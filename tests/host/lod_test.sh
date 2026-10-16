#!/bin/sh
# builds LOD meshes with tools/vcpe_lod.py (a UV sphere, and an OBJ round trip), checks the tool's numbers, then runs
# the host test against the sphere. vcpe_lod.c is compiled alone: it must not need any PSP header.
set -e
H=$(cd "$(dirname "$0")" && pwd); V=$(cd "$H/../.." && pwd)
python3 -I - "$V/tools" <<'P'
import sys, struct; sys.path.insert(0, sys.argv[1])
from vcpe_lod import build_lod, uv_sphere, read_obj
v, i = uv_sphere()
blob, info = build_lod(v, i)
open('/tmp/vcpe_lod_sphere.vld', 'wb').write(blob)
# true sphere: every vertex of the full mesh is on it, a coarser level must stay within its measured error
ok = info[-1][0] <= info[0][0] // 8 and all(e1 >= e0 for (_, _, e0), (_, _, e1) in zip(info, info[1:]))
print(('PASS' if ok else 'FAIL'), 'tool: %d -> %d triangles, errors %s' % (info[0][0], info[-1][0], ' '.join('%.4f' % e for _, _, e in info)))
# OBJ in: a quad grid with a seam-free top; the open border is locked, so the outline survives every level
with open('/tmp/vcpe_lod_grid.obj', 'w') as f:
    n = 16
    for y in range(n + 1):
        for x in range(n + 1):
            f.write('v %g %g %g\nvt %g %g\n' % (x, 0.02 * ((x * 7 + y * 3) % 5), y, x / n, y / n))
    for y in range(n):
        for x in range(n):
            a = y * (n + 1) + x + 1
            f.write('f %d/%d %d/%d %d/%d %d/%d\n' % (a, a, a + 1, a + 1, a + n + 2, a + n + 2, a + n + 1, a + n + 1))
gv, gi = read_obj('/tmp/vcpe_lod_grid.obj')
gb, ginfo = build_lod(gv, gi)
print(('PASS' if len(gv) == 289 and ginfo[0][0] == 512 and len(ginfo) >= 3 and ginfo[-1][2] < 0.2 else 'FAIL'),
      'tool: OBJ grid %d verts, levels %s' % (len(gv), ' '.join('%d/%.3f' % (t, e) for t, _, e in ginfo)))
P
gcc -O2 -Wall -Wextra -std=c99 -I"$V/src" "$H/lod_test.c" "$V/src/vcpe_lod.c" -lm -o /tmp/vcpe_lod_test
/tmp/vcpe_lod_test /tmp/vcpe_lod_sphere.vld
