"""GameMaker texture conversion for the VCPE GM runtime.

Every sprite, background and font image is cut out of the GM texture pages and repacked into 256x256 PSP pages.
Each image gets its own palette (T4 when it has <= 16 colours, else T8; > 256 colours are quantized), so a page holds
only indices and the runtime loads the image's CLUT when it draws. Images wider or taller than a page are split
into pieces. Pages are what the runtime caches (loaded from the PAK on first use).
"""
import io
import struct
import numpy as np
from PIL import Image
import imagequant

PAGE = 256


def swizzle(data, width_bytes, height):
    a = np.frombuffer(data, np.uint8).reshape(height // 8, 8, width_bytes // 16, 16).transpose(0, 2, 1, 3)
    return a.tobytes()


# Same skyline packer as vcpe_tex.Skyline, fixed to one 256x256 page.
class Shelf:
    """skyline packer on one page"""

    def __init__(self):
        self.sky = [(0, 0, PAGE)]

    def place(self, rw, rh):
        best = None
        for i, (x, y, w) in enumerate(self.sky):
            if x + rw > PAGE:
                break
            top, span, j = 0, 0, i
            while span < rw and j < len(self.sky):
                top = max(top, self.sky[j][1])
                span += self.sky[j][2]
                j += 1
            if span < rw or top + rh > PAGE:
                continue
            if best is None or top < best[1] or (top == best[1] and x < best[0]):
                best = (x, top)
        if best is None:
            return None
        x, y = best
        new = []
        for (sx, sy, sw) in self.sky:
            e = sx + sw
            if e <= x or sx >= x + rw:
                new.append((sx, sy, sw))
            else:
                if sx < x:
                    new.append((sx, sy, x - sx))
                if e > x + rw:
                    new.append((x + rw, sy, e - (x + rw)))
        new.append((x, y + rh, rw))
        new.sort()
        merged = []
        for seg in new:
            if merged and merged[-1][1] == seg[1] and merged[-1][0] + merged[-1][2] == seg[0]:
                merged[-1] = (merged[-1][0], seg[1], merged[-1][2] + seg[2])
            else:
                merged.append(seg)
        self.sky = merged
        return best


# RGBA pixels as one 32-bit number each, so whole frames can be compared and counted with numpy.
def rgba_u32(a):
    return (a[..., 0].astype(np.uint32) | (a[..., 1].astype(np.uint32) << 8) | (a[..., 2].astype(np.uint32) << 16) |
            (a[..., 3].astype(np.uint32) << 24))


class Image8:
    """one GM image (sprite / background / font): frames of RGBA crops with their offsets in the frame"""

    def __init__(self, name, frames):
        self.name = name
        self.frames = frames   # list of (ox, oy, rgba ndarray) or None


class TexBuilder:
    def __init__(self, gm):
        self.gm = gm
        self.src = []
        for t in gm.txtr:
            self.src.append(np.asarray(Image.open(io.BytesIO(t.png)).convert('RGBA')))
        self.pages = {4: [], 8: []}     # bpp -> list of (Shelf, ndarray indices)
        self.images = []                # per image: dict(pal=bytes, bpp, frames=[[pieces]])
        self.page_order = []            # final page list (bpp, local idx)

    def crop(self, tp):
        if tp is None:
            return None
        a = self.src[tp.page][tp.sy:tp.sy + tp.sh, tp.sx:tp.sx + tp.sw]
        if (tp.sw, tp.sh) != (tp.tw, tp.th):
            a = np.asarray(Image.fromarray(a).resize((tp.tw, tp.th), Image.NEAREST))
        return (tp.tx, tp.ty, a)

    def add(self, name, tpags):
        """returns the image index"""
        return self.add_frames(name, [self.crop(t) for t in tpags])

    # Convert one image (all its frames): find its colours, give it a palette, cut it into pieces and place them.
    # Palette index 0 is always transparent. Up to 15 colours: a 4-bit image (16-entry palette); up to 255: 8-bit with
    # the exact colours; more: all frames are stacked into one atlas and quantized together, so every frame shares one
    # palette. Frames bigger than a page are split into 256x256 pieces.
    def add_frames(self, name, frames):
        """Engine-independent RGBA frames: [(offset_x, offset_y, numpy_rgba), ...]."""
        crops = [f[2] for f in frames if f is not None and f[2].size]
        # transparent pixels all become 0
        cols = set()
        for c in crops:
            u = rgba_u32(c)
            u[(u >> 24) == 0] = 0
            cols.update(np.unique(u).tolist())
        cols.discard(0)
        cols = sorted(cols)
        if len(cols) <= 15:
            bpp = 4
        else:
            bpp = 8
        idx_frames = []
        if len(cols) <= 255:
            lut = {c: i + 1 for i, c in enumerate(cols)}
            pal = [0] + cols
            for c in crops:
                u = rgba_u32(c)
                u[(u >> 24) == 0] = 0
                flat = u.ravel()
                uniq, inv = np.unique(flat, return_inverse=True)
                m = np.array([lut.get(int(x), 0) for x in uniq], np.uint8)
                idx_frames.append(m[inv].reshape(u.shape))
        else:
            # quantize all frames together through one atlas
            hgt = sum(c.shape[0] + 1 for c in crops)
            wid = max(c.shape[1] for c in crops)
            at = np.zeros((hgt, wid, 4), np.uint8)
            y = 0
            for c in crops:
                at[y:y + c.shape[0], :c.shape[1]] = c
                y += c.shape[0] + 1
            q = imagequant.quantize_pil_image(Image.fromarray(at), dithering_level=0.0, max_colors=255)
            qp = q.getpalette('RGBA')
            qi = np.asarray(q, np.uint8)
            # put transparent at index 0
            palc = [struct.unpack('<I', bytes(qp[i * 4:i * 4 + 4]))[0] for i in range(len(qp) // 4)]
            remap = np.zeros(256, np.uint8)
            pal = [0]
            for i, c in enumerate(palc):
                if (c >> 24) == 0:
                    remap[i] = 0
                else:
                    remap[i] = len(pal)
                    pal.append(c)
            y = 0
            for c in crops:
                idx_frames.append(remap[qi[y:y + c.shape[0], :c.shape[1]]])
                y += c.shape[0] + 1
        n = 16 if bpp == 4 else 256
        pal = pal + [0] * (n - len(pal))
        palb = struct.pack('<%dI' % n, *pal)
        # pieces
        out_frames = []
        k = 0
        for f in frames:
            if f is None or not f[2].size:
                out_frames.append([])
                continue
            ox, oy, _ = f
            ind = idx_frames[k]
            k += 1
            pieces = []
            h, w = ind.shape
            for py in range(0, h, PAGE):
                for px in range(0, w, PAGE):
                    part = ind[py:py + PAGE, px:px + PAGE]
                    pg, u, v = self.place(bpp, part)
                    pieces.append((pg, u, v, part.shape[1], part.shape[0], ox + px, oy + py))
            out_frames.append(pieces)
        self.images.append(dict(name=name, pal=palb, bpp=bpp, frames=out_frames))
        return len(self.images) - 1

    # Put a piece on a page of its bit depth, with a 1-pixel gap (no bleeding with linear filtering). Only the last 4
    # pages are tried, so pieces of one image stay close together, then a new page is opened.
    def place(self, bpp, part):
        h, w = part.shape
        pw, ph = min(w + 1, PAGE), min(h + 1, PAGE)   # 1 px gap
        pages = self.pages[bpp]
        # try the last few pages first (locality), then a new page
        for i in range(max(0, len(pages) - 4), len(pages)):
            pos = pages[i][0].place(pw, ph)
            if pos:
                break
        else:
            pages.append((Shelf(), np.zeros((PAGE, PAGE), np.uint8)))
            i = len(pages) - 1
            pos = pages[i][0].place(pw, ph)
        x, y = pos
        pages[i][1][y:y + h, x:x + w] = part
        return (bpp, i), x, y

    # Number every page globally (8-bit pages first), pack 4-bit pages two pixels per byte (low nibble = left pixel),
    # swizzle everything, and rewrite the pieces' page references.
    def finish(self):
        """-> (page blobs list [(bpp, bytes)], images with global page numbers)"""
        glob = {}
        blobs = []
        for bpp in (8, 4):
            for i, (_, a) in enumerate(self.pages[bpp]):
                glob[(bpp, i)] = len(blobs)
                if bpp == 8:
                    raw = a.tobytes()
                    blobs.append((8, swizzle(raw, PAGE, PAGE)))
                else:
                    lo = a[:, 0::2] & 15
                    hi = a[:, 1::2] & 15
                    raw = (lo | (hi << 4)).astype(np.uint8).tobytes()
                    blobs.append((4, swizzle(raw, PAGE // 2, PAGE)))
        for im in self.images:
            im['frames'] = [[(glob[p[0]],) + p[1:] for p in fr] for fr in im['frames']]
        return blobs, self.images
