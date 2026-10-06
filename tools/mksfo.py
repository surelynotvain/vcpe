"""PARAM.SFO writer for the XMB: keys in alphabetical order, like Sony's own files.

The pspsdk only runs mksfoex when PARAM.SFO is missing, so each port's build.sh writes it first:
    import mksfo; mksfo.write(path, mksfo.entries("Title", "1.00", "UCJS10041"))"""
import struct

MAX_LEN = {"TITLE": 128, "DISC_ID": 16, "CATEGORY": 4}


def entries(title, version, disc_id, memsize=0, parental=7):
    return {
        "APP_VER": "0" + version if len(version) == 4 else version,
        "BOOTABLE": 1,
        "CATEGORY": "MG",
        "DISC_ID": disc_id,
        "DISC_VERSION": version,
        "MEMSIZE": memsize,           # 1 = ask for the PSP-2000+ extra RAM
        "PARENTAL_LEVEL": parental,
        "PSP_SYSTEM_VER": "1.00",
        "REGION": 32768,
        "TITLE": title,
    }


def build(ent):
    keys, data, index = b"", b"", b""
    for name in sorted(ent):
        v = ent[name]
        if isinstance(v, int):
            raw, fmt, used, cap = struct.pack("<I", v), 0x0404, 4, 4
        else:
            raw = v.encode("utf-8") + b"\0"
            cap = max(MAX_LEN.get(name, 8), (len(raw) + 3) & ~3)
            fmt, used = 0x0204, len(raw)
            raw = raw.ljust(cap, b"\0")
        index += struct.pack("<HHIII", len(keys), fmt, used, cap, len(data))
        keys += name.encode() + b"\0"
        data += raw
    keys = keys.ljust((len(keys) + 3) & ~3, b"\0")
    key_off = 20 + len(index)
    data_off = key_off + len(keys)
    head = struct.pack("<4sIIII", b"\0PSF", 0x0101, key_off, data_off, len(ent))
    return head + index + keys + data


def write(path, ent):
    open(path, "wb").write(build(ent))
