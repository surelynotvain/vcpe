"""VCPE texture helpers for the converters: power-of-two sizes, skyline page packing,
PSP swizzle, palette quantize (T4/T8 + RGBA CLUT) and 64-byte alignment."""
import numpy as np
import imagequant


def pow2(v):
    p = 8
    while p < v:
        p <<= 1
    return p


def pad64(b):
    while len(b) % 64:
        b += b"\0"
    return b


class Skyline:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.sky = [(0, 0, w)]  # (x, y, width)

    def place(self, rw, rh):
        best = None
        for i, (x, y, w) in enumerate(self.sky):
            if x + rw > self.w:
                break
            # max y across spanned segments
            top = 0
            span = 0
            j = i
            while span < rw and j < len(self.sky):
                top = max(top, self.sky[j][1])
                span += self.sky[j][2]
                j += 1
            if span < rw or top + rh > self.h:
                continue
            if best is None or top + rh < best[1] + rh or (top == best[1] and x < best[0]):
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
        # merge equal heights
        merged = []
        for seg in new:
            if merged and merged[-1][1] == seg[1] and merged[-1][0] + merged[-1][2] == seg[0]:
                merged[-1] = (merged[-1][0], seg[1], merged[-1][2] + seg[2])
            else:
                merged.append(seg)
        self.sky = merged
        return best


def swizzle(data, width_bytes, height):
    """PSP 16-byte x 8-row block swizzle."""
    a = np.frombuffer(data, np.uint8).reshape(height, width_bytes)
    a = a.reshape(height // 8, 8, width_bytes // 16, 16).transpose(0, 2, 1, 3)
    return a.tobytes()


def quantize(img, colors):
    """RGBA image -> (index array, RGBA palette list of colors*4)."""
    q = imagequant.quantize_pil_image(img, dithering_level=0.6 if colors == 256 else 0.4, max_colors=colors)
    pal = q.getpalette("RGBA") if hasattr(q, "getpalette") else None
    if pal is None or len(pal) < 4:
        raise RuntimeError("palette")
    pal = pal[: colors * 4] + [0] * max(0, colors * 4 - len(pal))
    return np.asarray(q, np.uint8), pal
