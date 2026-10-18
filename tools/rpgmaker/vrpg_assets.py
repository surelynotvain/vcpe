"""RPG Maker (VX / VX Ace) project assets -> VCPE data: sprites (VSPR section for vcpe_spr), streamed music
(vcpe_stream tracks), resident sound effects (vcpe_audio ADPCM) and the name tables the runtime looks names up in
(rpg_res_images / rpg_res_sounds, lowercase "folder/name" keys without extension).

    from vrpg_assets import Assets
    a = Assets(['Game', 'Patch'], cache='build/cache')          # later folders override earlier ones
    a.image_scale = {'pictures': 0.5667}                         # store big artwork smaller (the runtime scales back)
    a.hues = [('battlers', 'Slime', 120)]                        # hue-rotated copies RPG Maker makes at runtime
    sec, names, scales = a.sprites()
    se_blob, se_tab = a.sound_effects(adpcm)
    tracks = a.streams(adpcm, 'MUSIC.PAK', base=64)

Everything is cached per source file and option, so a rebuild only converts what changed."""
import hashlib
import os
import struct
import json
import subprocess
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
from gm.gmtex import TexBuilder, PAGE, rgba_u32   # noqa: E402
import vcpe_stream                                 # noqa: E402

IMAGE_EXT = ('.png', '.jpg', '.jpeg', '.bmp')
AUDIO_EXT = ('.ogg', '.mp3', '.wav', '.mid', '.midi', '.wma')


def key_of(folder, name):
    return (folder + '/' + os.path.splitext(name)[0]).lower()


def sha(path, extra=''):
    h = hashlib.sha1(extra.encode())
    with open(path, 'rb') as f:
        h.update(f.read())
    return h.hexdigest()


def hue_change(im, hue):
    """Bitmap#hue_change: rotate every pixel's hue by `hue` degrees (HSV, value and saturation kept)."""
    if not hue:
        return im
    a = np.asarray(im.convert('RGBA')).astype(np.float32) / 255.0
    rgb = a[..., :3]
    mx = rgb.max(-1)
    mn = rgb.min(-1)
    d = mx - mn
    h = np.zeros_like(mx)
    m = d > 1e-6
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    rm = m & (mx == r)
    gm = m & (mx == g) & ~rm
    bm = m & ~rm & ~gm
    h[rm] = ((g - b)[rm] / d[rm]) % 6
    h[gm] = (b - r)[gm] / d[gm] + 2
    h[bm] = (r - g)[bm] / d[bm] + 4
    h = (h * 60 + hue) % 360
    s = np.where(mx > 0, d / np.maximum(mx, 1e-6), 0)
    v = mx
    c = v * s
    x = c * (1 - np.abs((h / 60) % 2 - 1))
    z = np.zeros_like(c)
    sector = (h // 60).astype(int) % 6
    out = np.stack([np.choose(sector, [c, x, z, z, x, c]), np.choose(sector, [x, c, c, x, z, z]),
                    np.choose(sector, [z, z, x, c, c, x])], -1) + (v - c)[..., None]
    a[..., :3] = out
    return Image.fromarray((a * 255 + 0.5).clip(0, 255).astype(np.uint8), 'RGBA')


def windowskin_colors(path):
    """the 32 text colours of a VX Ace window skin (8x8 squares at 64, 96), as 0xAABBGGRR"""
    im = Image.open(path).convert('RGBA')
    out = []
    for n in range(32):
        r, g, b, a = im.getpixel((64 + (n % 8) * 8 + 4, 96 + (n // 8) * 8 + 4))
        out.append(r | g << 8 | b << 16 | 0xff << 24)
    return out


class Packer(TexBuilder):
    """TexBuilder fed with already indexed images (palette + index array), so quantizing can be cached."""

    def __init__(self):
        self.gm = None
        self.pages = {4: [], 8: []}
        self.images = []

    def add_indexed(self, name, ind, pal, bpp, ox=0, oy=0):
        pieces = []
        h, w = ind.shape
        for py in range(0, h, PAGE):
            for px in range(0, w, PAGE):
                part = ind[py:py + PAGE, px:px + PAGE]
                pg, u, v = self.place(bpp, part)
                pieces.append((pg, u, v, part.shape[1], part.shape[0], ox + px, oy + py))
        self.images.append(dict(name=name, pal=pal, bpp=bpp, frames=[pieces]))
        return len(self.images) - 1


def index_image(rgba, dither):
    """RGBA array -> (ox, oy, index array, palette bytes, bpp) with transparent borders trimmed"""
    alpha = rgba[..., 3] > 0
    ys = np.where(alpha.any(axis=1))[0]
    xs = np.where(alpha.any(axis=0))[0]
    if not len(ys):
        return 0, 0, np.zeros((1, 1), np.uint8), struct.pack('<16I', *([0] * 16)), 4
    ox, oy = int(xs[0]), int(ys[0])
    c = rgba[ys[0]:ys[-1] + 1, xs[0]:xs[-1] + 1]
    u = rgba_u32(c)
    u[(u >> 24) == 0] = 0
    cols = np.unique(u)
    cols = cols[cols != 0]
    if len(cols) <= 255:
        bpp = 4 if len(cols) <= 15 else 8
        pal = [0] + [int(x) for x in cols]
        lut = {int(x): i + 1 for i, x in enumerate(cols)}
        flat = u.ravel()
        uniq, inv = np.unique(flat, return_inverse=True)
        m = np.array([lut.get(int(x), 0) for x in uniq], np.uint8)
        ind = m[inv].reshape(u.shape)
    else:
        import imagequant
        bpp = 8
        q = imagequant.quantize_pil_image(Image.fromarray(c), dithering_level=dither, max_colors=255)
        qp = q.getpalette('RGBA')
        qi = np.asarray(q, np.uint8)
        palc = [struct.unpack('<I', bytes(qp[i * 4:i * 4 + 4]))[0] for i in range(len(qp) // 4)]
        remap = np.zeros(256, np.uint8)
        pal = [0]
        for i, col in enumerate(palc):
            if (col >> 24) == 0:
                remap[i] = 0
            else:
                remap[i] = len(pal)
                pal.append(col)
        ind = remap[qi]
    n = 16 if bpp == 4 else 256
    pal = pal + [0] * (n - len(pal))
    return ox, oy, ind, struct.pack('<%dI' % n, *pal), bpp


class Assets:
    def __init__(self, roots, cache):
        self.roots = list(roots)
        self.cache = cache
        os.makedirs(os.path.join(cache, 'img'), exist_ok=True)
        os.makedirs(os.path.join(cache, 'snd'), exist_ok=True)
        self.image_scale = {}      # folder -> scale (stored size / original size)
        self.dither = {}           # folder -> imagequant dithering level (photographic art)
        self.hues = []             # (folder, name, hue)
        self.skip = set()          # keys left out

    def files(self, top, exts):
        out = {}
        for r in self.roots:
            base = os.path.join(r, top)
            if not os.path.isdir(base):
                continue
            for folder in sorted(os.listdir(base)):
                d = os.path.join(base, folder)
                if not os.path.isdir(d):
                    continue
                for fn in sorted(os.listdir(d)):
                    if fn.lower().endswith(exts):
                        k = key_of(folder, fn)
                        if k not in self.skip:
                            out[k] = os.path.join(d, fn)
        return out

    def images(self):
        return self.files('Graphics', IMAGE_EXT)

    def audio(self):
        return self.files('Audio', AUDIO_EXT)

    # ------------------------------------------------------------------ sprites
    def _converted(self, key, path, hue):
        folder = key.split('/')[0]
        scale = self.image_scale.get(folder, 1.0)
        dither = self.dither.get(folder, 0.0)
        tag = '%s|%s|%s|%s|v1' % (key, hue, scale, dither)
        cp = os.path.join(self.cache, 'img', sha(path, tag) + '.npz')
        im = Image.open(path).convert('RGBA')
        w0, h0 = im.size
        if os.path.exists(cp):
            z = np.load(cp)
            return (w0, h0, int(z['ox']), int(z['oy']), z['ind'], bytes(z['pal']), int(z['bpp']), float(z['scale']),
                    tuple(int(x) for x in z['size']))
        if hue:
            im = hue_change(im, hue)
        if scale != 1.0:
            nw, nh = max(1, round(w0 * scale)), max(1, round(h0 * scale))
            im = im.resize((nw, nh), Image.LANCZOS)
        ox, oy, ind, pal, bpp = index_image(np.asarray(im), dither)
        np.savez(cp, ox=ox, oy=oy, ind=ind, pal=np.frombuffer(pal, np.uint8), bpp=bpp, scale=scale,
                 size=np.array(im.size))
        return w0, h0, ox, oy, ind, pal, bpp, scale, im.size

    def sprites(self, progress=None):
        """-> (VSPR section bytes, [(key, image id)] sorted, [scale per image id])"""
        imgs = self.images()
        entries = [(k, p, 0) for k, p in sorted(imgs.items())]
        for folder, name, hue in self.hues:
            k = key_of(folder, name)
            if k in imgs and hue:
                entries.append(('%s@%d' % (k, hue), imgs[k], hue))
        pk = Packer()
        names, scales, metas = [], [], []
        for i, (k, p, hue) in enumerate(entries):
            if progress and i % 50 == 0:
                progress(i, len(entries))
            w0, h0, ox, oy, ind, pal, bpp, scale, size = self._converted(k.split('@')[0], p, hue)
            idx = pk.add_indexed(k, ind, pal, bpp, ox, oy)
            names.append((k, idx))
            scales.append(scale)
            metas.append(size)
        blobs, ims = pk.finish()
        sec = self._section(blobs, ims, metas)
        names.sort(key=lambda t: t[0].encode('utf-8'))
        return sec, names, scales

    @staticmethod
    def _section(blobs, ims, metas):
        imgs = frames = pieces = pal = b''
        nframes = npieces = 0
        for im, (w, h) in zip(ims, metas):
            pal += b'\0' * (-len(pal) % 64)
            pal_off = len(pal)
            pal += im['pal']
            imgs += struct.pack('<HHhhhhhhHBBII', w, h, 0, 0, 0, 0, w - 1, h - 1, len(im['frames']), im['bpp'], 0,
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
        return bytes(sec)

    # ------------------------------------------------------------------ audio
    def sound_effects(self, adpcm, rate=22050, folders=('se',)):
        """-> (blob, [(key, off, size, samples)] sorted): mono ADPCM, resident"""
        from vcpe_snd import encode
        files = {k: p for k, p in self.audio().items() if k.split('/')[0] in folders}
        blob = bytearray()
        tab = []
        for k, p in sorted(files.items(), key=lambda t: t[0].encode('utf-8')):
            cp = os.path.join(self.cache, 'snd', sha(p, 'se%d' % rate) + '.bin')
            if os.path.exists(cp):
                data = open(cp, 'rb').read()
                n = struct.unpack_from('<I', data)[0]
                enc = data[4:]
            else:
                n, enc = encode(adpcm, p, rate, 1)
                open(cp, 'wb').write(struct.pack('<I', n) + enc)
            blob += b'\0' * (-len(blob) % 4)
            tab.append((k, len(blob), len(enc), n))
            blob += enc
        return bytes(blob), tab

    def streams(self, adpcm, folders, rates, loops, base=64, progress=None):
        """-> (StreamPak, [(key, track id)] sorted). rates[folder] = (rate, channels); loops[folder] = True / False"""
        sp = vcpe_stream.StreamPak(adpcm, base)
        files = {k: p for k, p in self.audio().items() if k.split('/')[0] in folders}
        ids = []
        for i, (k, p) in enumerate(sorted(files.items(), key=lambda t: t[0].encode('utf-8'))):
            if progress and i % 20 == 0:
                progress(i, len(files))
            folder = k.split('/')[0]
            rate, ch = rates[folder]
            loop = loops[folder]
            cp = os.path.join(self.cache, 'snd', sha(p, 'st%d%d%s' % (rate, ch, loop)) + '.bin')
            cached = None
            if os.path.exists(cp):
                data = open(cp, 'rb').read()
                isamp, ilen, lsamp = struct.unpack_from('<3I', data)
                cached = (data[12:12 + ilen], isamp, data[12 + ilen:], lsamp)
            t = sp.add(k, p, rate, ch, loop, cached)
            if not cached:
                trk = sp.tracks[t]
                ib = bytes(sp.data[trk[0] - base:trk[2] - base])
                lb = bytes(sp.data[trk[2] - base:])
                open(cp, 'wb').write(struct.pack('<3I', trk[1], len(ib), trk[3]) + ib + lb)
            ids.append((k, t))
        return sp, ids


def c_names(var, rows):
    """a sorted RpgName table"""
    out = ['const RpgName %s[] = {' % var]
    for k, i in rows:
        out.append('    {%s, %d},' % (json.dumps(k, ensure_ascii=False), i))
    out.append('    {0, 0}};')
    return '\n'.join(out)
