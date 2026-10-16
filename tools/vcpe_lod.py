"""VCPE LOD meshes (src/vcpe_lod.c): one mesh, several detail levels, chosen at runtime by how big their error looks
on screen. Far away, neighbouring triangles are merged ("joined") as long as the change stays under about a pixel.

    from vcpe_lod import build_lod, read_obj
    verts, idx = read_obj('crate.obj')                 # verts: [(u, v, abgr, x, y, z)], idx: [i0, i1, i2, ...]
    blob, info = build_lod(verts, idx)                 # info: [(triangles, vertices, error)] per level
    # put blob in the PAK or a pack (vcpe_packer, kind data); on the PSP: vcpe_lod_open(&mesh, mem, size)

How the levels are made: half-edge collapses ranked by quadric error (Garland-Heckbert). A collapse u -> v moves
every triangle of u onto v and drops the two that shared the edge; no new vertices are made, so every level uses a
subset of the original vertices. The vertices are reordered so that each level only uses a prefix of the vertex
array (vertices collapsed late come before those collapsed early). Collapses that would flip a triangle, pinch the
surface (link condition) or move an open edge or a UV/colour seam are refused, so outlines and texture seams stay put.

A level's error is measured, not estimated: the largest distance between the original surface and the level, in both
directions (original vertices to the level's triangles, and points sampled on the level's triangles to the original
triangles). The runtime turns it into pixels with the camera's distance and field of view.

Layout (little endian, 16-byte aligned): header {"VLD1", u16 vertices, u16 levels, float centre[3], float radius}
(24 bytes, padded to 32), the level table {u16 vertices used, u16 0, u32 indices, u32 index offset, float error}
(16 bytes each, level 0 = full detail, error 0), the vertices (VcpeVtx: float u, v, u32 ABGR, float x, y, z), then
each level's u16 indices on a 16-byte boundary.
Command line: vcpe_lod.py IN.obj OUT.vld [--levels N] [--ratio R] [--min-tris T]"""
import heapq
import math
import struct
import sys

import numpy as np

MAGIC = b'VLD1'


def read_obj(path, color=0xFFFFFFFF):
    """Wavefront OBJ (v, vt, f; polygons fanned; v with r g b gives vertex colours) -> (verts, idx).
    A vertex is a distinct (position, uv, colour) corner, as the GE needs; V is flipped (OBJ is bottom-up)."""
    pos, col, uvs, verts, idx, seen = [], [], [], [], [], {}
    for line in open(path, encoding='utf-8', errors='replace'):
        p = line.split()
        if not p:
            continue
        if p[0] == 'v':
            pos.append(tuple(map(float, p[1:4])))
            if len(p) >= 7:
                r, g, b = (max(0, min(255, int(float(c) * 255 + 0.5))) for c in p[4:7])
                col.append(0xFF000000 | b << 16 | g << 8 | r)
            else:
                col.append(color)
        elif p[0] == 'vt':
            uvs.append((float(p[1]), 1.0 - float(p[2]) if len(p) > 2 else 0.0))
        elif p[0] == 'f':
            corners = []
            for c in p[1:]:
                f = c.split('/')
                vi = int(f[0]) - 1 if int(f[0]) > 0 else len(pos) + int(f[0])
                ti = (int(f[1]) - 1 if int(f[1]) > 0 else len(uvs) + int(f[1])) if len(f) > 1 and f[1] else -1
                key = (vi, ti)
                if key not in seen:
                    u, v = uvs[ti] if ti >= 0 else (0.0, 0.0)
                    seen[key] = len(verts)
                    verts.append((u, v, col[vi]) + pos[vi])
                corners.append(seen[key])
            for k in range(1, len(corners) - 1):
                idx += [corners[0], corners[k], corners[k + 1]]
    return verts, idx


def _plane_quadric(a, b, c):
    n = np.cross(b - a, c - a)
    area2 = np.linalg.norm(n)
    if area2 < 1e-12:
        return None
    n /= area2
    p = np.append(n, -np.dot(n, a))
    return np.outer(p, p) * (area2 * 0.5)   # area-weighted


def _quad_cost(q, p):
    h = np.append(p, 1.0)
    return max(0.0, float(h @ q @ h))


def _point_tri_dist(p, a, b, c):
    """distance from points p (N,3) to every triangle (a,b,c each (M,3)) -> (N,) minimum. Ericson's closest point."""
    best = np.full(len(p), np.inf)
    for s in range(0, len(a), 512):
        A, B, C = a[s:s + 512][None], b[s:s + 512][None], c[s:s + 512][None]
        P = p[:, None]
        ab, ac, ap = B - A, C - A, P - A
        d1, d2 = (ab * ap).sum(-1), (ac * ap).sum(-1)
        bp = P - B
        d3, d4 = (ab * bp).sum(-1), (ac * bp).sum(-1)
        cp = P - C
        d5, d6 = (ab * cp).sum(-1), (ac * cp).sum(-1)
        va, vb, vc = d3 * d6 - d5 * d4, d5 * d2 - d1 * d6, d1 * d4 - d3 * d2
        den = va + vb + vc
        den = np.where(np.abs(den) < 1e-20, 1e-20, den)
        v, w = vb / den, vc / den
        q = A + ab * v[..., None] + ac * w[..., None]              # inside the face
        def seg(X, Y):                                              # closest on edge X-Y
            xy = Y - X
            t = np.clip(((P - X) * xy).sum(-1) / np.maximum((xy * xy).sum(-1), 1e-20), 0, 1)
            return X + xy * t[..., None]
        inside = (va >= 0) & (vb >= 0) & (vc >= 0)
        cands = [seg(A, B), seg(B, C), seg(C, A)]
        dd = [((P - x) ** 2).sum(-1) for x in cands]
        de = np.minimum(np.minimum(dd[0], dd[1]), dd[2])
        di = ((P - q) ** 2).sum(-1)
        d = np.where(inside, np.minimum(di, de), de)
        best = np.minimum(best, d.min(1))
    return np.sqrt(best)


def _near_dist(p, a, b, c):
    """_point_tri_dist, but each point only against triangles near it: triangles are bucketed by their bounding
    boxes in a grid of cell h; a point's nearest triangle within h always overlaps the point's cell grown by h, so
    those results are exact; points with nothing that close fall back to every triangle."""
    if len(p) * len(a) < 4e6:
        return _point_tri_dist(p, a, b, c)
    lo = np.minimum(np.minimum(a, b), c)
    hi = np.maximum(np.maximum(a, b), c)
    h = max(float(np.median((hi - lo).max(1))) * 2, 1e-6)
    origin = np.minimum(lo.min(0), p.min(0))
    clo = np.floor((lo - origin) / h).astype(np.int64)
    chi = np.floor((hi - origin) / h).astype(np.int64)
    pc = np.floor((p - origin) / h).astype(np.int64)
    buckets = {}
    for t in range(len(a)):
        for x in range(clo[t, 0], chi[t, 0] + 1):
            for y in range(clo[t, 1], chi[t, 1] + 1):
                for z in range(clo[t, 2], chi[t, 2] + 1):
                    buckets.setdefault((x, y, z), []).append(t)
    out = np.full(len(p), np.inf)
    groups = {}
    for i, k in enumerate(map(tuple, pc)):
        groups.setdefault(k, []).append(i)
    for (x, y, z), pts in groups.items():
        cand = set()
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for dz in (-1, 0, 1):
                    cand.update(buckets.get((x + dx, y + dy, z + dz), ()))
        if cand:
            ci = np.fromiter(cand, np.int64)
            out[pts] = _point_tri_dist(p[pts], a[ci], b[ci], c[ci])
    far = ~(out <= h)
    if far.any():
        out[far] = _point_tri_dist(p[far], a, b, c)
    return out


def _surface_error(P, tri_full, tri_lvl):
    """two-sided distance between the full mesh and a level (both triangle index arrays over positions P)"""
    if len(tri_lvl) == 0:
        return float('inf')
    used = np.unique(tri_full)
    e1 = _near_dist(P[used], *(P[tri_lvl[:, k]] for k in range(3))).max()
    # points on the level's triangles: centroids and edge midpoints
    a, b, c = (P[tri_lvl[:, k]] for k in range(3))
    samples = np.concatenate([(a + b + c) / 3, (a + b) / 2, (b + c) / 2, (c + a) / 2])
    e2 = _near_dist(samples, *(P[tri_full[:, k]] for k in range(3))).max()
    return float(max(e1, e2))


def simplify(verts, idx, levels=5, ratio=0.5, min_tris=8):
    """-> (order, level list [(vertex count, index list in new vertex numbers, error)]) """
    nv = len(verts)
    P = np.array([v[3:6] for v in verts], dtype=np.float64)
    tris = [tuple(idx[i:i + 3]) for i in range(0, len(idx) - len(idx) % 3, 3)]
    tris = [t for t in tris if len(set(t)) == 3]
    tri_full = np.array(tris, dtype=np.int64).reshape(-1, 3)

    # topology works on welded positions: copies of one corner at a UV/colour seam are one point
    weld, wid = {}, []
    for v in verts:
        wid.append(weld.setdefault(tuple(round(c, 6) for c in v[3:6]), len(weld)))
    copies = {}
    for i, w in enumerate(wid):
        copies.setdefault(w, []).append(i)
    edge_count = {}
    for t in tris:
        for k in range(3):
            a, b = wid[t[k]], wid[t[(k + 1) % 3]]
            e = (min(a, b), max(a, b))
            edge_count[e] = edge_count.get(e, 0) + 1
    locked = [False] * nv
    for (a, b), n in edge_count.items():
        if n != 2:                                   # open edge or non-manifold: keep the outline
            for w in (a, b):
                for i in copies[w]:
                    locked[i] = True
    for w, cs in copies.items():
        if len(cs) > 1:                              # seam: collapsing one copy would tear the texture
            for i in cs:
                locked[i] = True

    Q = [np.zeros((4, 4)) for _ in range(nv)]
    for t in tris:
        q = _plane_quadric(P[t[0]], P[t[1]], P[t[2]])
        if q is not None:
            for i in t:
                Q[i] += q
    vt = [set() for _ in range(nv)]                  # live triangles of each vertex
    live = {}
    for ti, t in enumerate(tris):
        live[ti] = list(t)
        for i in t:
            vt[i].add(ti)

    def neighbours(u):
        s = set()
        for ti in vt[u]:
            s.update(live[ti])
        s.discard(u)
        return s

    def collapse_ok(u, v):
        if locked[u]:
            return False
        nu, nvv = neighbours(u), neighbours(v)
        shared = [ti for ti in vt[u] if v in live[ti]]
        if len(nu & nvv) != len(shared):             # link condition: no pinched surface
            return False
        for ti in vt[u]:
            if ti in shared:
                continue
            t = live[ti]
            a, b, c = (P[i] for i in t)
            n0 = np.cross(b - a, c - a)
            t2 = [v if i == u else i for i in t]
            a, b, c = (P[i] for i in t2)
            n1 = np.cross(b - a, c - a)
            l0, l1 = np.linalg.norm(n0), np.linalg.norm(n1)
            if l1 < 1e-12 or np.dot(n0, n1) < 0.2 * l0 * l1:   # flipped or folded
                return False
        return True

    stamp = [0] * nv
    heap = []

    def push(u):
        stamp[u] += 1
        if locked[u]:
            return
        for v in neighbours(u):
            uvd = (verts[u][0] - verts[v][0]) ** 2 + (verts[u][1] - verts[v][1]) ** 2
            cost = _quad_cost(Q[u] + Q[v], P[v]) + 1e-6 * uvd
            heapq.heappush(heap, (cost, u, v, stamp[u]))

    for u in range(nv):
        push(u)

    n_tris = len(tris)
    targets = []
    t = n_tris
    for _ in range(levels - 1):
        t = int(t * ratio)
        if t < min_tris:
            break
        targets.append(t)
    collapsed, snapshots = [], []                    # collapsed: vertices in collapse order
    for target in targets:
        while n_tris > target and heap:
            cost, u, v, st = heapq.heappop(heap)
            if st != stamp[u] or not vt[u] or v not in neighbours(u) or not collapse_ok(u, v):
                continue
            for ti in list(vt[u]):
                t3 = live[ti]
                if v in t3:                          # the edge's triangles disappear
                    for i in t3:
                        vt[i].discard(ti)
                    del live[ti]
                    n_tris -= 1
                else:
                    live[ti] = [v if i == u else i for i in t3]
                    vt[v].add(ti)
            vt[u].clear()
            Q[v] += Q[u]
            collapsed.append(u)
            for w in neighbours(v) | {v}:
                push(w)
        if n_tris > target * 1.15 and snapshots and n_tris >= snapshots[-1][1]:
            break                                    # nothing more can be joined
        snapshots.append((len(collapsed), n_tris, [tuple(t3) for t3 in live.values()]))

    gone = set(collapsed)
    order = [i for i in range(nv) if i not in gone] + collapsed[::-1]
    new = {old: k for k, old in enumerate(order)}
    out = [(nv, [new[i] for t3 in tris for i in t3], 0.0)]
    for ncol, _, lt in snapshots:
        err = _surface_error(P, tri_full, np.array(lt, dtype=np.int64).reshape(-1, 3))
        err = max(err, out[-1][2])                   # coarser levels never claim less error
        out.append((nv - ncol, [new[i] for t3 in lt for i in t3], err))
    return order, out


def build_lod(verts, idx, levels=5, ratio=0.5, min_tris=8):
    """-> (VLD1 bytes, [(triangles, vertices, error)] per level)"""
    if len(verts) > 65535:
        raise ValueError('a VCPE mesh has at most 65535 vertices (16-bit indices)')
    order, lv = simplify(verts, idx, levels, ratio, min_tris)
    P = np.array([v[3:6] for v in verts], dtype=np.float64)
    used = np.unique(np.array(idx, dtype=np.int64)) if idx else np.arange(len(verts))
    lo, hi = P[used].min(0), P[used].max(0)
    centre = (lo + hi) / 2
    radius = float(np.sqrt(((P[used] - centre) ** 2).sum(1)).max())
    head = MAGIC + struct.pack('<HH4f', len(verts), len(lv), *centre, radius)
    head += b'\0' * (32 - len(head))
    vert_off = 32 + 16 * len(lv)
    vert_off += -vert_off % 16
    vdata = b''.join(struct.pack('<2fI3f', *verts[o]) for o in order)
    table, idata = b'', b''
    for nvu, il, err in lv:
        pad = -(vert_off + len(vdata) + len(idata)) % 16
        idata += b'\0' * pad
        table += struct.pack('<HHIIf', nvu, 0, len(il), vert_off + len(vdata) + len(idata), err)
        idata += struct.pack('<%dH' % len(il), *il)
    blob = head + table + b'\0' * (vert_off - 32 - len(table)) + vdata + idata
    blob += b'\0' * (-len(blob) % 16)
    return blob, [(len(il) // 3, nvu, err) for nvu, il, err in lv]


def uv_sphere(rings=24, segs=48, r=1.0, color=0xFFFFFFFF):
    """test mesh: a UV sphere with a texture seam (closed, so the poles and seam are the only locked parts)"""
    verts, idx = [], []
    for i in range(rings + 1):
        th = math.pi * i / rings
        for j in range(segs + 1):
            ph = 2 * math.pi * j / segs
            verts.append((j / segs, i / rings, color, r * math.sin(th) * math.cos(ph), r * math.cos(th), r * math.sin(th) * math.sin(ph)))
    w = segs + 1
    for i in range(rings):
        for j in range(segs):
            a, b, c, d = i * w + j, i * w + j + 1, (i + 1) * w + j, (i + 1) * w + j + 1
            if i > 0:
                idx += [a, b, c]
            if i < rings - 1:
                idx += [b, d, c]
    return verts, idx


def main(argv):
    if len(argv) < 2:
        print(__doc__.strip().splitlines()[-1])
        return 2
    opts = {'--levels': 5, '--ratio': 0.5, '--min-tris': 8}
    rest = []
    it = iter(argv)
    for a in it:
        if a in opts:
            opts[a] = type(opts[a])(next(it))
        else:
            rest.append(a)
    verts, idx = read_obj(rest[0])
    blob, info = build_lod(verts, idx, opts['--levels'], opts['--ratio'], opts['--min-tris'])
    open(rest[1], 'wb').write(blob)
    for k, (t, v, e) in enumerate(info):
        print('level %d: %6d triangles, %5d vertices, error %.5g' % (k, t, v, e))
    print('%s: %d bytes' % (rest[1], len(blob)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
