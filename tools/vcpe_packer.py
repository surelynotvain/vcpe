"""VCPE asset packs for compact loading (src/vcpe_pack.c): many assets in one block that loads with one read.

    from vcpe_packer import build_pack
    blob = build_pack([('tree.tex', tex_bytes), ('wind.adp', snd_bytes, KIND_SOUND)])
    # put blob in the PAK (vcpe_pak.write_pak), then on the PSP: vcpe_pack_load(PAK_FOREST_OFF, PAK_FOREST_SIZE, 60)

Layout (little endian): a 64-byte header {"VPK1", version 1, total size, entry count, table offset}, the entry
table {FNV-1a hash of the name, offset, size, kind, flags} sorted by hash (the PSP finds entries by binary
search), then every asset on a 64-byte boundary, so textures can be handed to the GE where they lie.
Command line: vcpe_packer.py OUT.vpk FILE... (each file named by its base name)."""
import os
import struct
import sys

KIND_DATA, KIND_TEXTURE, KIND_SOUND, KIND_TEXT = 0, 1, 2, 3


def fnv(name):
    """FNV-1a 32-bit, the same function as vcpe_fnv() / the pack loader"""
    h = 2166136261
    for b in name.encode('utf-8'):
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def build_pack(entries):
    """entries: [(name, bytes) or (name, bytes, kind) or (name, bytes, kind, flags)] -> pack bytes"""
    items = []
    for e in entries:
        name, data = e[0], bytes(e[1])
        kind = e[2] if len(e) > 2 else KIND_DATA
        flags = e[3] if len(e) > 3 else 0
        items.append((fnv(name), name, data, kind, flags))
    items.sort(key=lambda t: t[0])
    for a, b in zip(items, items[1:]):
        if a[0] == b[0]:
            raise ValueError('names %r and %r have the same hash: rename one' % (a[1], b[1]))
    table_off = 64
    data_off = table_off + 16 * len(items)
    data_off += -data_off % 64                       # the first asset starts on a 64-byte boundary
    table, body, pos = bytearray(), bytearray(), data_off
    for h, _, data, kind, flags in items:
        table += struct.pack('<IIIHH', h, pos, len(data), kind, flags)
        body += data + b'\0' * (-len(data) % 64)    # the next asset stays aligned
        pos = data_off + len(body)
    total = data_off + len(body)
    head = struct.pack('<4sIIII', b'VPK1', 1, total, len(items), table_off).ljust(64, b'\0')
    out = head + table
    out += b'\0' * (data_off - len(out))
    return bytes(out + body)


if __name__ == '__main__':
    if len(sys.argv) < 3:
        sys.exit('usage: vcpe_packer.py OUT.vpk FILE...')
    blob = build_pack([(os.path.basename(p), open(p, 'rb').read()) for p in sys.argv[2:]])
    open(sys.argv[1], 'wb').write(blob)
    print('%s: %d assets, %d bytes' % (sys.argv[1], len(sys.argv) - 2, len(blob)))
