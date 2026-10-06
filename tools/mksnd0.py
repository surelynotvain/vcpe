"""Rewrap an atracdenc ATRAC3 RIFF as SND0.AT3 (the XMB plays it while the game is highlighted).

Usage: mksnd0.py in.at3 pcm_samples SND0.AT3
  in.at3:      atracdenc -e atrac3_lp4 --container=riff (66 kbps, 192-byte frames)
  pcm_samples: length of the source WAV in samples

The XMB plays nothing when fact promises more audio than the frames hold. Sony's at3tool files
keep fact.samples + fact.delay + 368 <= frames * 1024; atracdenc writes frames * 1024, so the
count is clamped here. A one-loop smpl chunk (start = delay, end = delay + samples - 1) follows,
as in Sony's files."""
import struct, sys

SPF = 1024          # ATRAC3 samples per frame
DELAY = 1024        # fact delay / first sample offset
TAIL = 368


def chunks(b):
    pos, out = 12, {}
    while pos + 8 <= len(b):
        cid, size = b[pos:pos + 4], struct.unpack_from("<I", b, pos + 4)[0]
        out[cid] = b[pos + 8:pos + 8 + size]
        pos += 8 + size + (size & 1)
    return out


def main(src, pcm_samples, dst):
    c = chunks(open(src, "rb").read())
    fmt, data = c[b"fmt "], c[b"data"]
    tag, _, _, _, align = struct.unpack_from("<HHIIH", fmt)
    if tag != 0x0270:
        sys.exit("not ATRAC3 (fmt tag %#x)" % tag)
    frames = len(data) // align
    samples = min(pcm_samples, frames * SPF - DELAY - TAIL)
    fact = struct.pack("<II", samples, DELAY)
    smpl = struct.pack("<9I", 0, 0, 22676, 60, 0, 0, 0, 1, 0x18) + struct.pack("<6I", 0, 0, DELAY, DELAY + samples - 1, 0, 0)
    body = b"WAVE"
    for cid, payload in ((b"fmt ", fmt), (b"fact", fact), (b"smpl", smpl), (b"data", data)):
        body += cid + struct.pack("<I", len(payload)) + payload + (b"\0" if len(payload) & 1 else b"")
    open(dst, "wb").write(b"RIFF" + struct.pack("<I", len(body)) + body)
    print("%s: %d frames of %d bytes, fact %d samples (+%d delay), %d bytes"
          % (dst, frames, align, samples, DELAY, 8 + len(body)))


if __name__ == "__main__":
    main(sys.argv[1], int(sys.argv[2]), sys.argv[3])
