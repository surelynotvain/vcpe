"""2 x 2 terrain cells through tools/vcpe_terrain.py, with reference heights and level errors for tests/terrain."""
import math
import os
import sys

import numpy as np

V = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
sys.path.insert(0, os.path.join(V, 'tools'))
import vcpe_terrain as T  # noqa: E402

out = sys.argv[1]
N, SIZE = 16, 32.0
side = 2 * N + 1
quad = SIZE / N
ys, xs = np.mgrid[0:side, 0:side] * quad
h = 10 + 5 * np.sin(xs / 9.0) * np.cos(ys / 7.0) + np.where(xs > 40, 6.0, 0.0)   # rolling ground and a cliff
h[0:5, 0:5] = 3.0                                                                    # a flat corner
colors = np.zeros((8, 8, 3), np.uint8)
colors[..., 0] = 200
colors[..., 1] = 160
colors[..., 2] = 100
overlay = np.zeros((side, side))
overlay[:, 10:14] = 1.0                                                              # a road strip
cells, info = T.cut(h, N, SIZE, colors, overlay, page=3, overlay_page=4, uv=4.0)
for cx, cz, blob in cells:
    open(os.path.join(out, 'cell_%d_%d.vtr' % (cx, cz)), 'wb').write(blob)
far, fw, fd = T.far_grid(h, N, 2, info)
open(os.path.join(out, 'far.u16'), 'wb').write(far)

base, step = info['base'], info['step']
q = np.clip(np.round((h - base) / step), 0, 65535)
hq = base + step * q


def tri(qi, qj, s, u, v):
    a, b, c, d = hq[qj, qi], hq[qj, qi + s], hq[qj + s, qi], hq[qj + s, qi + s]
    return a + (b - a) * u + (d - b) * v if u >= v else a + (c - a) * v + (d - c) * u


lines = ['far %d %d' % (fw, fd)]
rng = np.random.default_rng(5)
for _ in range(200):
    x, z = rng.uniform(0, 2 * SIZE, 2)
    lx, lz = x / quad, z / quad
    qi, qj = min(int(lx), 2 * N - 1), min(int(lz), 2 * N - 1)
    lines.append('h %.9g %.9g %.9g' % (x, z, tri(qi, qj, 1, lx - qi, lz - qj)))
for cz in range(2):
    for cx in range(2):
        for lvl in range(1, 5):
            s = 1 << lvl
            e = 0.0
            for j in range(N + 1):
                for i in range(N + 1):
                    if i % s == 0 and j % s == 0:
                        continue
                    qi, qj = min(i // s * s, N - s), min(j // s * s, N - s)
                    gi, gj = cx * N, cz * N
                    e = max(e, abs(hq[gj + j, gi + i] - tri(gi + qi, gj + qj, s, (i - qi) / s, (j - qj) / s)))
            lines.append('err %d %d %d %.9g' % (cx, cz, lvl, e))
open(os.path.join(out, 'expect.txt'), 'w').write('\n'.join(lines) + '\n')
