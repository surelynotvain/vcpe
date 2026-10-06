"""Mux an H.264 elementary stream into ICON1.PMF (the XMB's animated icon).

Usage: mkicon1.py icon.264 ICON1.PMF

The stream must already be in the XMB icon regime: 144x80, 29.97 fps, Main profile level 2.1,
no B-frames, an access unit delimiter on every frame, 600 kbps HRD (see make_icon1.sh).

Layout (Sony icon PMFs): a 0x800-byte PSMF header, then 2048-byte MPEG-2 program stream packs.
Every IDR group of pictures is a chunk that starts on a pack boundary with a private-stream-2
frame size table, followed by video PES packets; leftover pack space becomes a padding PES."""
import re, struct, sys

TICK = 3003            # 90 kHz ticks per frame at 29.97 fps
PTS0 = 90000
PACK = 2048
PACK_HDR = 14
MUX_RATE = 25000       # 50-byte units per second
PREFILL = 76850        # SCR of the first pack sits this many ticks before the first DTS
W, H = 144, 80
SYSTEM_HEADER = bytes.fromhex("000001bb000c80c35180f07fb9e050bde008")   # 80 KB video P-STD bound
PSTD_EXT = bytes.fromhex("1e6050")                                       # PES extension: P-STD 80 KB


def be16(v): return struct.pack(">H", v)
def be32(v): return struct.pack(">I", v)


def access_units(es):
    """Split at AUDs; drop filler NALs and the encoder's user-data SEI. -> [(bytes, is_idr)]"""
    marks = [m.start() for m in re.finditer(b"\x00\x00\x01", es)]
    aus, cur, idr = [], bytearray(), False
    for i, s in enumerate(marks):
        start = s - 1 if s > 0 and es[s - 1] == 0 else s          # keep 4-byte start codes whole
        end = marks[i + 1] if i + 1 < len(marks) else len(es)
        if i + 1 < len(marks) and es[end - 1] == 0:
            end -= 1
        nal = es[s + 3:end]
        kind = nal[0] & 31
        if kind == 9 and cur:
            aus.append((bytes(cur), idr))
            cur, idr = bytearray(), False
        if kind == 12 or (kind == 6 and len(nal) > 1 and nal[1] == 5):
            continue
        cur += es[start:end]
        idr |= kind == 5
    if cur:
        aus.append((bytes(cur), idr))
    if not aus or not aus[0][1]:
        sys.exit("stream must start with an IDR access unit")
    return aus


def ts33(prefix, v):
    return bytes([prefix | ((v >> 29) & 0x0E) | 1, (v >> 22) & 0xFF, ((v >> 14) & 0xFE) | 1,
                  (v >> 7) & 0xFF, ((v << 1) & 0xFE) | 1])


def scr(v27):
    base, ext = divmod(v27, 300)
    return bytes([0x44 | ((base >> 27) & 0x38) | ((base >> 28) & 0x03), (base >> 20) & 0xFF,
                  ((base >> 12) & 0xF8) | 0x04 | ((base >> 13) & 0x03), (base >> 5) & 0xFF,
                  ((base << 3) & 0xF8) | 0x04 | ((ext >> 7) & 0x03), ((ext << 1) & 0xFE) | 1])


def size_table(sizes, first):
    out = bytearray(b"\x01\xe0")
    for v in ((0, 0, 1, 1, 0, 0) if first else (0, 1, 1, 2, 0, 0)):
        out += be16(v)
    out += be16(2 + 4 * len(sizes)) + be16(len(sizes))
    for i, n in enumerate(sizes):
        out += be16(0 if i == len(sizes) - 1 else 0x80) + be16(n)
    return bytes(out)


def mux(aus):
    chunks = []
    for i, (_, idr) in enumerate(aus):
        if idr or not chunks:
            chunks.append([])
        chunks[-1].append(i)
    pts = [PTS0 + TICK * i for i in range(len(aus))]
    stream, last_stamp = bytearray(), 0
    for ci, chunk in enumerate(chunks):
        es = b"".join(aus[i][0] for i in chunk)
        starts, acc = [], 0
        for i in chunk:
            starts.append(acc)
            acc += len(aus[i][0])
        pos, first = 0, True
        while pos < len(es):
            pack = bytearray(b"\x00\x00\x01\xba" + bytes(6)   # SCR patched below; mux rate, no stuffing
                             + bytes([(MUX_RATE >> 14) & 0xFF, (MUX_RATE >> 6) & 0xFF, ((MUX_RATE << 2) & 0xFC) | 3, 0xF8]))
            room = PACK - PACK_HDR
            if ci == 0 and first:
                pack += SYSTEM_HEADER
                room -= len(SYSTEM_HEADER)
            if first:
                tab = size_table([len(aus[i][0]) for i in chunk], ci == 0)
                pack += b"\x00\x00\x01\xbf" + be16(len(tab)) + tab
                room -= 6 + len(tab)

            def au_in(n):   # first AU (index into chunk) that starts inside the next n payload bytes
                return next((k for k, o in enumerate(starts) if pos <= o < pos + n), None)

            # timestamps go on a chunk's first PES and whenever 16+ frames passed since the last
            hl = 0
            for _ in range(3):
                n = min(len(es) - pos, room - 9 - hl)
                k = au_in(n)
                want = (13 if first else 10) if (first or (k is not None and chunk[k] >= last_stamp + 16)) else 0
                if want == hl:
                    break
                hl = want
            n = min(len(es) - pos, room - 9 - hl)
            k = au_in(n)
            fields, flags = b"", 0
            if hl and k is not None:
                p = pts[chunk[k]]
                fields = ts33(0x30, p) + ts33(0x10, p - TICK)
                flags = 0xC0
                if hl == 13:
                    fields += PSTD_EXT
                    flags = 0xC1
                last_stamp = chunk[k]
            spare = room - 9 - hl - n
            stuff = spare if 0 < spare < 6 else 0          # too small for a padding PES
            hlen = hl + stuff
            pack += (b"\x00\x00\x01\xe0" + be16(3 + hlen + n) + bytes([0x81, flags, hlen])
                     + fields + b"\xff" * (hlen - len(fields)) + es[pos:pos + n])
            pos += n
            spare -= stuff
            if spare:
                pack += b"\x00\x00\x01\xbe" + be16(spare - 6) + b"\xff" * (spare - 6)
            assert len(pack) == PACK, len(pack)
            stream += pack
            first = False
    # SCR: linear ramp starting before the first DTS
    npacks = len(stream) // PACK
    scr0 = (pts[0] - TICK - PREFILL) * 300
    step = max(44237, min(TICK * 300, len(aus) * TICK * 300 // max(1, npacks)))
    for i in range(npacks):
        stream[i * PACK + 4:i * PACK + 10] = scr(scr0 + i * step)
    last = PTS0 + TICK * len(aus)
    hdr = bytearray(0x800)
    hdr[0:8] = b"PSMF0014"
    hdr[8:12] = be32(0x800)
    hdr[12:16] = be32(len(stream))
    hdr[0x50:0x54] = be32(0x3E)
    hdr[0x54:0x5A] = PTS0.to_bytes(6, "big")
    hdr[0x5A:0x60] = last.to_bytes(6, "big")
    hdr[0x60:0x64] = be32(MUX_RATE)
    hdr[0x64:0x68] = be32(90000)
    hdr[0x68:0x6A] = b"\x01\x01"
    hdr[0x6A:0x6E] = be32(0x24)
    hdr[0x6E:0x74] = PTS0.to_bytes(6, "big")
    hdr[0x74:0x7A] = last.to_bytes(6, "big")
    hdr[0x7A:0x7C] = be16(1)
    hdr[0x7C:0x80] = be32(0x12)
    hdr[0x80:0x82] = be16(1)
    hdr[0x82:0x92] = bytes([0xE0, 0x00, 0x20, 0x50]) + bytes(8) + bytes([W // 16, H // 16, 0, 0])
    return bytes(hdr) + bytes(stream), len(chunks)


def demux(pmf):
    """Video elementary stream back out of the packs (self-check)."""
    out, s = bytearray(), pmf[0x800:]
    for p in range(0, len(s), PACK):
        q = p + PACK_HDR
        while q < p + PACK:
            sid, ln = s[q + 3], struct.unpack(">H", s[q + 4:q + 6])[0]
            if sid == 0xE0:
                out += s[q + 9 + s[q + 8]:q + 6 + ln]
            q += 6 + ln
    return bytes(out)


if __name__ == "__main__":
    es = open(sys.argv[1], "rb").read()
    aus = access_units(es)
    pmf, nchunks = mux(aus)
    assert demux(pmf) == b"".join(a for a, _ in aus), "demux round trip differs"
    open(sys.argv[2], "wb").write(pmf)
    print("%s: %d frames, %d chunks, %d bytes" % (sys.argv[2], len(aus), nchunks, len(pmf)))
