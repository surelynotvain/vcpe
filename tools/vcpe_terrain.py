"""VCPE terrain cells (src/vcpe_terrain.c): cut one big heightmap into VTR1 cell blobs plus a coarse far grid.

    from vcpe_terrain import cut, far_grid, load_heightmap
    h = load_heightmap('world.png')                              # 2D float array, metres after --height scaling
    cells, info = cut(h, n=32, size=64.0)                        # [(cx, cz, VTR1 bytes)], quantisation info
    far, fw, fd = far_grid(h, n=32, per_cell=2, info=info)       # u16 heights for vcpe_terrain_far_init

The heightmap needs (cells x n + 1) samples per side: neighbouring cells share their edge samples, so their edges
match exactly. Heights use one base/step for the whole world (u16), normals are computed across cell borders (no
lighting seams) and stored with each cell. A colour map (any size; resampled) gives vertex colours; an overlay mask
gives the vertex alpha that paints the cell's overlay texture (roads, snow) over its base texture.

Command line:
  vcpe_terrain.py HEIGHTMAP OUTDIR [--n 32] [--size 64] [--height MIN MAX] [--color MAP] [--overlay MASK]
                  [--page N] [--overlay-page N] [--uv REPEATS] [--far SAMPLES_PER_CELL]
HEIGHTMAP: 8/16-bit greyscale PNG/PGM (scaled to --height MIN..MAX), .npy (metres) or .raw (square u16 LE, scaled).
Writes OUTDIR/cell_<cx>_<cz>.vtr, OUTDIR/far.u16 and OUTDIR/terrain.h (sizes and quantisation constants)."""
import math
import os
import struct
import sys

import numpy as np

HEADER = struct.Struct('<4sHHiifffhhfIII')   # 48 bytes


def load_heightmap(path, lo=0.0, hi=1000.0):
    ext = os.path.splitext(path)[1].lower()
    if ext == '.npy':
        return np.load(path).astype(np.float64)
    if ext == '.raw':
        raw = np.frombuffer(open(path, 'rb').read(), dtype='<u2')
        side = int(round(math.sqrt(raw.size)))
        if side * side != raw.size:
            raise ValueError('.raw heightmaps must be square u16')
        return lo + raw.reshape(side, side).astype(np.float64) / 65535.0 * (hi - lo)
    from PIL import Image
    im = Image.open(path)
    a = np.asarray(im)
    if a.ndim == 3:
        a = a[..., 0]
    top = 65535.0 if a.dtype == np.uint16 or a.max() > 255 else 255.0
    return lo + a.astype(np.float64) / top * (hi - lo)


def _resample(img, w, h):
    """nearest-neighbour-free bilinear resample of an (H, W[, C]) array to (h, w[, C])"""
    a = np.asarray(img, dtype=np.float64)
    H, W = a.shape[:2]
    ys = np.linspace(0, H - 1, h)
    xs = np.linspace(0, W - 1, w)
    y0 = np.floor(ys).astype(int)
    x0 = np.floor(xs).astype(int)
    y1 = np.minimum(y0 + 1, H - 1)
    x1 = np.minimum(x0 + 1, W - 1)
    fy = (ys - y0)[:, None]
    fx = (xs - x0)[None, :]
    if a.ndim == 3:
        fy, fx = fy[..., None], fx[..., None]
    top = a[y0][:, x0] * (1 - fx) + a[y0][:, x1] * fx
    bot = a[y1][:, x0] * (1 - fx) + a[y1][:, x1] * fx
    return top * (1 - fy) + bot * fy


def normals(h, quad):
    """per-sample normals (x y z, unit) from central differences over the whole map"""
    gz, gx = np.gradient(h, quad)
    n = np.stack([-gx, np.ones_like(h), -gz], axis=-1)
    return n / np.linalg.norm(n, axis=-1, keepdims=True)


def quantise(h):
    lo, hi = float(h.min()), float(h.max())
    step = (hi - lo) / 65535.0 if hi > lo else 1.0 / 65535.0
    return lo, step, np.clip(np.round((h - lo) / step), 0, 65535).astype(np.uint16)


def cut(h, n=32, size=64.0, colors=None, overlay=None, page=0, overlay_page=-1, uv=8.0):
    """h: (cells_z * n + 1, cells_x * n + 1) heights in metres. colors: (H, W, 3|4) 0..255, overlay: (H, W) 0..1.
    -> ([(cx, cz, blob)], {'base', 'step', 'cells_x', 'cells_z', 'n', 'size'})"""
    h = np.asarray(h, dtype=np.float64)
    if n < 2 or n > 64 or n & (n - 1):
        raise ValueError('n must be a power of two, 2..64')
    rows, cols = h.shape
    if (rows - 1) % n or (cols - 1) % n:
        raise ValueError('heightmap needs cells x %d + 1 samples per side (got %dx%d)' % (n, cols, rows))
    cz_n, cx_n = (rows - 1) // n, (cols - 1) // n
    base, step, q = quantise(h)
    nrm = np.clip(np.round(normals(h, size / n) * 127), -127, 127).astype(np.int8)
    col = None
    if colors is not None or overlay is not None:
        col = np.full((rows, cols, 4), 255.0)
        if colors is not None:
            c = _resample(colors, cols, rows)
            col[..., :c.shape[-1]] = c[..., :4]
        col[..., 3] = 0 if overlay is None else np.clip(_resample(overlay, cols, rows), 0, 1) * 255
        col = np.clip(np.round(col), 0, 255).astype(np.uint32)
        col = col[..., 0] | col[..., 1] << 8 | col[..., 2] << 16 | col[..., 3] << 24
    out = []
    s1 = n + 1
    for cz in range(cz_n):
        for cx in range(cx_n):
            ys, xs = slice(cz * n, cz * n + s1), slice(cx * n, cx * n + s1)
            hb = q[ys, xs].astype('<u2').tobytes()
            nb = np.concatenate([nrm[ys, xs], np.zeros((s1, s1, 1), np.int8)], axis=-1).tobytes()
            cb = col[ys, xs].astype('<u4').tobytes() if col is not None else b''
            heights = 48
            colors_at = heights + len(hb)
            colors_at += -colors_at % 16
            normals_at = colors_at + len(cb)
            normals_at += -normals_at % 16
            blob = bytearray(normals_at + len(nb))
            blob[0:48] = HEADER.pack(b'VTR1', n, 0, cx, cz, size, base, step, page, overlay_page, uv,
                                     colors_at if cb else 0, heights, normals_at)
            blob[heights:heights + len(hb)] = hb
            if cb:
                blob[colors_at:colors_at + len(cb)] = cb
            blob[normals_at:normals_at + len(nb)] = nb
            blob += b'\0' * (-len(blob) % 16)
            out.append((cx, cz, bytes(blob)))
    return out, {'base': base, 'step': step, 'cells_x': cx_n, 'cells_z': cz_n, 'n': n, 'size': size}


def far_grid(h, n, per_cell=1, info=None):
    """coarse u16 heights for vcpe_terrain_far_init: per_cell samples per cell side, same quantisation as the cells
    (pass cut()'s info). -> (bytes, quads across, quads down)"""
    h = np.asarray(h, dtype=np.float64)
    stride = n // per_cell
    if stride < 1 or n % per_cell:
        raise ValueError('per_cell must divide n')
    sub = h[::stride, ::stride]
    if info is None:
        base, step, _ = quantise(h)
    else:
        base, step = info['base'], info['step']
    q = np.clip(np.round((sub - base) / step), 0, 65535).astype('<u2')
    d, w = q.shape[0] - 1, q.shape[1] - 1
    if (w + 1) * (d + 1) > 65535:
        raise ValueError('far grid too large for 16-bit indices: use fewer samples per cell')
    return q.tobytes(), w, d


def main(argv):
    if len(argv) < 2:
        print('\n'.join(__doc__.strip().splitlines()[-5:]))
        return 2
    opts = {'--n': 32, '--size': 64.0, '--page': 0, '--overlay-page': -1, '--uv': 8.0, '--far': 1,
            '--color': None, '--overlay': None}
    lo, hi, rest = 0.0, 1000.0, []
    it = iter(argv)
    for a in it:
        if a == '--height':
            lo, hi = float(next(it)), float(next(it))
        elif a in opts:
            v = next(it)
            opts[a] = v if opts[a] is None else type(opts[a])(v)
        else:
            rest.append(a)
    h = load_heightmap(rest[0], lo, hi)
    colors = overlay = None
    if opts['--color'] or opts['--overlay']:
        from PIL import Image
        if opts['--color']:
            colors = np.asarray(Image.open(opts['--color']).convert('RGB'))
        if opts['--overlay']:
            overlay = np.asarray(Image.open(opts['--overlay']).convert('L')) / 255.0
    cells, info = cut(h, opts['--n'], opts['--size'], colors, overlay, opts['--page'], opts['--overlay-page'], opts['--uv'])
    os.makedirs(rest[1], exist_ok=True)
    for cx, cz, blob in cells:
        open(os.path.join(rest[1], 'cell_%d_%d.vtr' % (cx, cz)), 'wb').write(blob)
    far, fw, fd = far_grid(h, opts['--n'], opts['--far'], info)
    open(os.path.join(rest[1], 'far.u16'), 'wb').write(far)
    quad = opts['--size'] / opts['--far']
    hdr = ['/* generated by vcpe_terrain.py - do not edit */', '#ifndef TERRAIN_INFO_H', '#define TERRAIN_INFO_H',
           '#define TERRAIN_CELLS_X %d' % info['cells_x'], '#define TERRAIN_CELLS_Z %d' % info['cells_z'],
           '#define TERRAIN_CELL_N %d' % info['n'], '#define TERRAIN_CELL_SIZE %.9gf' % info['size'],
           '#define TERRAIN_HEIGHT_BASE %.9gf' % info['base'], '#define TERRAIN_HEIGHT_STEP %.9gf' % info['step'],
           '#define TERRAIN_FAR_W %d' % fw, '#define TERRAIN_FAR_D %d' % fd, '#define TERRAIN_FAR_QUAD %.9gf' % quad,
           '#endif', '']
    open(os.path.join(rest[1], 'terrain.h'), 'w').write('\n'.join(hdr))
    print('%d x %d cells of %d quads (%g m), heights %.2f..%.2f m (step %.4g), far grid %d x %d, %d bytes per cell' %
          (info['cells_x'], info['cells_z'], info['n'], info['size'], info['base'], info['base'] + 65535 * info['step'],
           info['step'], fw, fd, len(cells[0][2])))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
