"""Load uncompressed/RLE indexed, true-colour and greyscale TGA into Pillow RGBA.

Supports 16/24/32-bit palettes, 8/16-bit indices, 16/24/32-bit colour pixels,
8/16-bit greyscale and all four image origins. Rejects truncated packets and
out-of-range palette indices instead of returning a partially decoded image.
"""
import struct
from pathlib import Path
from PIL import Image


def load(path):
    data = Path(path).read_bytes()
    if len(data) < 18:
        raise ValueError("TGA header is truncated")
    ident, cmap, kind, first, count, pal_bits, _, _, width, height, bits, desc = struct.unpack(
        "<BBBHHBHHHHBB", data[:18])
    base = kind - 8 if kind in (9, 10, 11) else kind
    if base not in (1, 2, 3) or not width or not height or desc & 0xC0:
        raise ValueError("Unsupported TGA type, dimensions or interleaving")
    if cmap not in (0, 1) or (base == 1 and cmap != 1):
        raise ValueError("Invalid TGA colour map")
    if (base == 1 and bits not in (8, 16)) or (base == 2 and bits not in (16, 24, 32)) or (base == 3 and bits not in (8, 16)):
        raise ValueError("Unsupported TGA pixel depth")
    pos = 18

    def take(n):
        nonlocal pos
        if pos + n > len(data):
            raise ValueError("TGA data is truncated")
        part = data[pos:pos+n]
        pos += n
        return part

    def colour(raw, depth, alpha):
        if depth == 16:
            v = int.from_bytes(raw, "little")
            r, g, b = (v >> 10) & 31, (v >> 5) & 31, v & 31
            return bytes(((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2),
                          255 if not alpha or v & 0x8000 else 0))
        b, g, r = raw[:3]
        return bytes((r, g, b, raw[3] if depth == 32 else 255))

    take(ident)
    palette = []
    if cmap:
        if pal_bits not in (16, 24, 32) or not count:
            raise ValueError("Unsupported TGA palette")
        for _ in range(count):
            palette.append(colour(take(pal_bits // 8), pal_bits, True))
    pixel_bytes = bits // 8
    total = width * height
    pixels = bytearray()
    decoded = 0
    while decoded < total:
        header = take(1)[0] if kind >= 9 else 0
        length = (header & 127) + 1 if kind >= 9 else total - decoded
        if decoded + length > total:
            raise ValueError("TGA packet exceeds image dimensions")
        repeated = kind >= 9 and header & 128
        raw = take(pixel_bytes) if repeated else take(pixel_bytes * length)
        for i in range(length):
            value = raw if repeated else raw[i*pixel_bytes:(i+1)*pixel_bytes]
            if base == 1:
                index = int.from_bytes(value, "little") - first
                if index < 0 or index >= len(palette):
                    raise ValueError("TGA palette index is out of range")
                pixels.extend(palette[index])
            elif base == 2:
                pixels.extend(colour(value, bits, bool(desc & 15)))
            else:
                pixels.extend((value[0], value[0], value[0], value[1] if bits == 16 else 255))
        decoded += length
    result = Image.frombytes("RGBA", (width, height), bytes(pixels))
    if not desc & 32:
        result = result.transpose(Image.Transpose.FLIP_TOP_BOTTOM)
    if desc & 16:
        result = result.transpose(Image.Transpose.FLIP_LEFT_RIGHT)
    return result
