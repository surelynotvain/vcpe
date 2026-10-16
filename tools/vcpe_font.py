"""Bake a TTF/OTF font into a VCPE glyph page: white glyphs with alpha, for vcpe_font.c.

    img, glyphs, height = bake(path, px, chars)
img: 'L' image (alpha), width 256 (or 512), power-of-two height; glyphs: [(codepoint, u, v, w, h, xoff, yoff, adv)]."""
from PIL import Image, ImageDraw, ImageFont


def pow2(v):
    p = 8
    while p < v:
        p <<= 1
    return p


# Rasterize every character with Pillow and pack them left to right in rows of the font's line height (1-2 px gaps
# keep linear filtering from bleeding between glyphs). Each glyph records where it is (u, v, w, h), its offset from
# the pen position (xoff, yoff from the bounding box) and its advance.
def bake(path, px, chars, width=256):
    font = ImageFont.truetype(path, px)
    asc, desc = font.getmetrics()
    glyphs, placed = [], []
    x = y = 0
    row_h = asc + desc + 2
    for ch in sorted(set(chars)):
        cp = ord(ch)
        bbox = font.getbbox(ch)
        adv = int(round(font.getlength(ch)))
        w, h = max(0, bbox[2] - bbox[0]), max(0, bbox[3] - bbox[1])
        if x + w + 1 > width:
            x, y = 0, y + row_h
        placed.append((ch, x, y, bbox))
        glyphs.append((cp, x, y, w, h, bbox[0], bbox[1], adv))
        x += w + 2
    img = Image.new("L", (width, pow2(y + row_h)), 0)
    d = ImageDraw.Draw(img)
    for ch, gx, gy, bbox in placed:
        d.text((gx - bbox[0], gy - bbox[1]), ch, font=font, fill=255)
    return img, glyphs, asc + desc
