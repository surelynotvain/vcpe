"""VCPE sprite packer for any engine: RGBA images (numpy arrays or PIL images) -> the "VSPR" section vcpe_spr reads.

Each image gets its own palette (T4 when it has <= 15 colours, else T8, quantized with imagequant above 255) and is
packed with the others into 256x256 pages; transparent borders are trimmed per frame. This is the GameMaker
packer (gm/gmtex.py) with plain images in place of GameMaker texture pages.

    from vcpe_sprpack import SprPack
    sp = SprPack()
    i = sp.add('sword', [rgba])                    # origin (0, 0); add(..., ox=, oy=) for another origin
    j = sp.add('walk', [f0, f1, f2], ox=8, oy=16)  # frames of one image share the size and palette
    section = sp.build()                           # bytes for the PAK; vcpe_spr_init(section offset, budget)
"""
import struct

import numpy as np
from PIL import Image

from gm.gmtex import TexBuilder


# Accept PIL images or numpy arrays, always return an RGBA uint8 array.
def as_rgba(im):
    if isinstance(im, Image.Image):
        return np.asarray(im.convert('RGBA'))
    a = np.asarray(im)
    if a.ndim == 3 and a.shape[2] == 4:
        return a.astype(np.uint8)
    return np.asarray(Image.fromarray(a).convert('RGBA'))


# Cut away fully transparent rows and columns around a frame (they would waste page space), remembering where the rest
# starts.
def trim(a):
    """-> (x, y, cropped) of the non-transparent part, or None when the frame is empty"""
    alpha = a[..., 3] > 0
    ys = np.where(alpha.any(axis=1))[0]
    xs = np.where(alpha.any(axis=0))[0]
    if not len(ys):
        return None
    return int(xs[0]), int(ys[0]), a[ys[0]:ys[-1] + 1, xs[0]:xs[-1] + 1]


# TexBuilder does the palette and page work; this subclass feeds it plain images and writes the VSPR tables.
class SprPack(TexBuilder):
    def __init__(self):
        self.gm = None
        self.src = []
        self.pages = {4: [], 8: []}
        self.images = []
        self.meta = []

    def crop(self, frame):
        return frame   # frames are already (x, y, rgba) crops

    def add(self, name, frames, ox=0, oy=0):
        """frames: RGBA images of one size; returns the image index"""
        arrs = [as_rgba(f) for f in frames]
        h, w = arrs[0].shape[:2]
        idx = TexBuilder.add(self, name, [trim(a) for a in arrs])
        self.meta.append((w, h, ox, oy))
        return idx

    # Write the section: header (magic + 15 counts and offsets), then images, frames, pieces, pages, (no fonts, no
    # glyphs), palettes and the page data, each 64-byte aligned. Row layouts match the C structs in src/vcpe_spr.h.
    def build(self):
        blobs, ims = self.finish()
        imgs = frames = pieces = pal = b''
        nframes = npieces = 0
        for im, (w, h, ox, oy) in zip(ims, self.meta):
            pal_off = len(pal)
            pal += im['pal']
            imgs += struct.pack('<HHhhhhhhHBBII', w, h, ox, oy, 0, 0, w - 1, h - 1, len(im['frames']), im['bpp'], 0,
                                nframes, pal_off)
            for fr in im['frames']:
                frames += struct.pack('<IHH', npieces, len(fr), 0)
                nframes += 1
                for (pg, u, v, pw, ph, dx, dy) in fr:
                    pieces += struct.pack('<HBBHHhh', pg, u, v, pw, ph, dx, dy)
                    npieces += 1
        pages = page_data = b''
        for bpp, raw in blobs:
            pages += struct.pack('<IB3x', len(page_data), bpp)
            page_data += raw
        sec = bytearray(4 + 15 * 4)
        offs = {}
        for name, blob in (('images', imgs), ('frames', frames), ('pieces', pieces), ('pages', pages), ('fonts', b''),
                           ('glyphs', b''), ('pal', pal)):
            sec += b'\0' * (-len(sec) % 64)
            offs[name] = len(sec)
            sec += blob
        sec += b'\0' * (-len(sec) % 64)
        offs['page_data'] = len(sec)
        sec += page_data
        struct.pack_into('<4s15I', sec, 0, b'VSPR', len(ims), nframes, npieces, len(blobs), 0, 0,
                         offs['images'], offs['frames'], offs['pieces'], offs['pages'], offs['fonts'], offs['glyphs'],
                         offs['pal'], len(pal), offs['page_data'])
        self.stats = dict(images=len(ims), frames=nframes, pieces=npieces, pages=len(blobs), page_bytes=len(page_data))
        return bytes(sec)
