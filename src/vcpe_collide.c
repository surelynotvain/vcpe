/* VCPE collide: grid-accelerated triangle meshes, cylinders, ground, rays and a capsule controller (see
 * vcpe_collide.h). */

/* HOW THIS FILE WORKS
 *
 * Meshes: triangles are binned into a uniform grid over the mesh's X/Z footprint (a triangle goes into every cell
 * its X/Z box touches). A query visits the cells it overlaps; a per-triangle stamp makes sure a triangle in several
 * cells is tested once. Rays walk the cells in order (Amanatides-Woo) and stop as soon as the best hit lies before
 * the cell being left. Instances keep a matrix and its inverse; queries move into mesh space and results back.
 *
 * The character is a capsule whose lower sphere floats step_up above the feet. Below it a ground probe (five
 * vertical rays: the centre and four points at 60 % of the radius) finds the highest walkable surface within
 * step_up above and step_down below the feet. Consequences, all on purpose:
 *   - steps and small rocks never block: the capsule passes over them and the probe lifts the feet on top;
 *   - walking down stairs or a slope snaps to the ground (within step_down) instead of skipping into the air;
 *   - a jump or fall never snaps: snapping happens only when the character was grounded and not moving up;
 *   - walls push the capsule out along their normal, so it slides along them; walkable surfaces that the capsule
 *     touches lift it straight up instead of pushing it back (no creeping backwards on ramps); steeper surfaces push
 *     it away and down, so it slides off.
 * Penetration is resolved triangle by triangle (each push moves the capsule before the next test), over at most four
 * passes per substep; substeps are half a radius long, so nothing thinner than that is tunnelled through.
 *
 * Capsule against triangle: the exact closest points between a segment and a triangle, which are among: the
 * segment crossing the triangle (distance 0), an endpoint and its closest triangle point, or a segment-edge pair. */
#include <math.h>
#include <string.h>
#include "vcpe_collide.h"

static float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void sub3(float o[3], const float a[3], const float b[3]) { o[0] = a[0] - b[0]; o[1] = a[1] - b[1]; o[2] = a[2] - b[2]; }
static void cross3(float o[3], const float a[3], const float b[3])
{
    float x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
    o[0] = x; o[1] = y; o[2] = z;
}
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static const float *vp(const VcpeColMesh *m, int i) { return (const float *)((const uint8_t *)m->pos + (size_t)i * (size_t)m->stride); }
static void xf_point(const float m[12], const float p[3], float o[3])
{
    float x = m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3], y = m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7];
    float z = m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11];
    o[0] = x; o[1] = y; o[2] = z;
}
static void xf_vec(const float m[12], const float v[3], float o[3])
{
    float x = m[0] * v[0] + m[1] * v[1] + m[2] * v[2], y = m[4] * v[0] + m[5] * v[1] + m[6] * v[2];
    float z = m[8] * v[0] + m[9] * v[1] + m[10] * v[2];
    o[0] = x; o[1] = y; o[2] = z;
}

/* ------------------------------------------------------------------ meshes */
typedef struct { float lo[3], hi[3], cell; int gw, gd; uint32_t refs; } Layout;

static int layout(const float *pos, int stride, const uint16_t *idx, int ntris, float cell, Layout *L)
{
    if (!pos || !idx || ntris < 1 || stride < 12 || !(cell > 0))
        return -1;
    for (int k = 0; k < 3; k++) { L->lo[k] = 3e38f; L->hi[k] = -3e38f; }
    VcpeColMesh tmp = {.pos = pos, .stride = stride};
    for (int t = 0; t < ntris * 3; t++) {
        const float *p = vp(&tmp, idx[t]);
        for (int k = 0; k < 3; k++) {
            if (!(p[k] == p[k])) return -1;
            if (p[k] < L->lo[k]) L->lo[k] = p[k];
            if (p[k] > L->hi[k]) L->hi[k] = p[k];
        }
    }
    float wx = L->hi[0] - L->lo[0], wz = L->hi[2] - L->lo[2];
    if (wx / 64 > cell) cell = wx / 64;
    if (wz / 64 > cell) cell = wz / 64;
    L->cell = cell;
    L->gw = (int)(wx / cell) + 1;
    L->gd = (int)(wz / cell) + 1;
    if (L->gw > 65) L->gw = 65;
    if (L->gd > 65) L->gd = 65;
    L->refs = 0;
    for (int t = 0; t < ntris; t++) {
        float lo[2] = {3e38f, 3e38f}, hi[2] = {-3e38f, -3e38f};
        for (int v = 0; v < 3; v++) {
            const float *p = vp(&tmp, idx[t * 3 + v]);
            lo[0] = fminf(lo[0], p[0]); hi[0] = fmaxf(hi[0], p[0]);
            lo[1] = fminf(lo[1], p[2]); hi[1] = fmaxf(hi[1], p[2]);
        }
        int x0 = (int)((lo[0] - L->lo[0]) / cell), x1 = (int)((hi[0] - L->lo[0]) / cell);
        int z0 = (int)((lo[1] - L->lo[2]) / cell), z1 = (int)((hi[1] - L->lo[2]) / cell);
        x1 = x1 >= L->gw ? L->gw - 1 : x1;
        z1 = z1 >= L->gd ? L->gd - 1 : z1;
        L->refs += (uint32_t)((x1 - x0 + 1) * (z1 - z0 + 1));
    }
    return 0;
}

uint32_t vcpe_colmesh_bytes(const float *pos, int stride, const uint16_t *idx, int ntris, float cell)
{
    Layout L;
    if (layout(pos, stride, idx, ntris, cell, &L))
        return 0;
    return (uint32_t)ntris * 20u + (uint32_t)(L.gw * L.gd + 1) * 4u + L.refs * 4u;
}

int vcpe_colmesh_build(VcpeColMesh *m, const float *pos, int stride, const uint16_t *idx, int ntris, float cell,
                       void *mem, uint32_t cap)
{
    Layout L;
    memset(m, 0, sizeof *m);
    if (layout(pos, stride, idx, ntris, cell, &L) || !mem || ((uintptr_t)mem & 3) ||
        cap < (uint32_t)ntris * 20u + (uint32_t)(L.gw * L.gd + 1) * 4u + L.refs * 4u)
        return -1;
    m->pos = pos;
    m->stride = stride;
    m->idx = idx;
    m->ntris = ntris;
    m->plane = mem;
    m->stamp = (uint32_t *)(m->plane + ntris);
    m->start = m->stamp + ntris;
    m->refs = m->start + L.gw * L.gd + 1;
    m->gw = L.gw;
    m->gd = L.gd;
    m->cell = L.cell;
    m->gx0 = L.lo[0];
    m->gz0 = L.lo[2];
    memcpy(m->lo, L.lo, sizeof m->lo);
    memcpy(m->hi, L.hi, sizeof m->hi);
    memset(m->start, 0, (size_t)(L.gw * L.gd + 1) * 4);
    for (int pass = 0; pass < 2; pass++) {
        /* pass 0 counts per cell, pass 1 fills (start[c] used as a cursor, then restored) */
        for (int t = 0; t < ntris; t++) {
            const float *a = vp(m, idx[t * 3]), *b = vp(m, idx[t * 3 + 1]), *c = vp(m, idx[t * 3 + 2]);
            float e1[3], e2[3], n[3];
            sub3(e1, b, a);
            sub3(e2, c, a);
            cross3(n, e1, e2);
            float l = sqrtf(dot3(n, n));
            if (pass == 0) {
                if (l < 1e-12f) { m->plane[t][0] = m->plane[t][1] = m->plane[t][2] = m->plane[t][3] = 0; }
                else {
                    for (int k = 0; k < 3; k++) m->plane[t][k] = n[k] / l;
                    m->plane[t][3] = dot3(m->plane[t], a);
                }
                m->stamp[t] = 0;
            }
            if (l < 1e-12f)
                continue;
            float lx = fminf(a[0], fminf(b[0], c[0])), hx = fmaxf(a[0], fmaxf(b[0], c[0]));
            float lz = fminf(a[2], fminf(b[2], c[2])), hz = fmaxf(a[2], fmaxf(b[2], c[2]));
            int x0 = (int)((lx - m->gx0) / m->cell), x1 = (int)((hx - m->gx0) / m->cell);
            int z0 = (int)((lz - m->gz0) / m->cell), z1 = (int)((hz - m->gz0) / m->cell);
            x1 = x1 >= m->gw ? m->gw - 1 : x1;
            z1 = z1 >= m->gd ? m->gd - 1 : z1;
            for (int z = z0; z <= z1; z++)
                for (int x = x0; x <= x1; x++) {
                    int cidx = z * m->gw + x;
                    if (pass == 0) m->start[cidx + 1]++;
                    else m->refs[m->start[cidx]++] = (uint32_t)t;
                }
        }
        if (pass == 0)
            for (int c = 0; c < m->gw * m->gd; c++) m->start[c + 1] += m->start[c];
        else {
            for (int c = m->gw * m->gd; c > 0; c--) m->start[c] = m->start[c - 1];
            m->start[0] = 0;
        }
    }
    return 0;
}

/* visit each triangle overlapping a mesh-space X/Z box once */
typedef int (*TriFn)(void *ctx, VcpeColMesh *m, int tri);
static void mesh_box(VcpeColMesh *m, const float lo[3], const float hi[3], TriFn fn, void *ctx)
{
    if (hi[0] < m->lo[0] || lo[0] > m->hi[0] || hi[1] < m->lo[1] || lo[1] > m->hi[1] || hi[2] < m->lo[2] || lo[2] > m->hi[2])
        return;
    int x0 = (int)floorf((lo[0] - m->gx0) / m->cell), x1 = (int)floorf((hi[0] - m->gx0) / m->cell);
    int z0 = (int)floorf((lo[2] - m->gz0) / m->cell), z1 = (int)floorf((hi[2] - m->gz0) / m->cell);
    if (x0 < 0) x0 = 0;
    if (z0 < 0) z0 = 0;
    if (x1 >= m->gw) x1 = m->gw - 1;
    if (z1 >= m->gd) z1 = m->gd - 1;
    uint32_t q = ++m->query;
    if (!q) { memset(m->stamp, 0, (size_t)m->ntris * 4); q = m->query = 1; }
    for (int z = z0; z <= z1; z++)
        for (int x = x0; x <= x1; x++) {
            int c = z * m->gw + x;
            for (uint32_t r = m->start[c]; r < m->start[c + 1]; r++) {
                uint32_t t = m->refs[r];
                if (m->stamp[t] == q) continue;
                m->stamp[t] = q;
                if (fn(ctx, m, (int)t)) return;
            }
        }
}

/* two-sided ray/triangle (Moller-Trumbore); t along d */
static int ray_tri(const float o[3], const float d[3], const float *a, const float *b, const float *c, float *t)
{
    float e1[3], e2[3], p[3], s[3], q[3];
    sub3(e1, b, a);
    sub3(e2, c, a);
    cross3(p, d, e2);
    float det = dot3(e1, p);
    if (fabsf(det) < 1e-12f) return 0;
    float inv = 1.0f / det;
    sub3(s, o, a);
    float u = dot3(s, p) * inv;
    if (u < 0 || u > 1) return 0;
    cross3(q, s, e1);
    float v = dot3(d, q) * inv;
    if (v < 0 || u + v > 1) return 0;
    *t = dot3(e2, q) * inv;
    return *t >= 0;
}

static int mesh_ray(VcpeColMesh *m, const float o[3], const float d[3], float max, float *best, int *best_tri)
{
    /* the part of the ray over the grid's X/Z rectangle */
    float t0 = 0, t1 = max;
    for (int k = 0; k < 3; k++) {
        if (fabsf(d[k]) < 1e-12f) {
            if (o[k] < m->lo[k] || o[k] > m->hi[k]) return 0;
            continue;
        }
        float a = (m->lo[k] - o[k]) / d[k], b = (m->hi[k] - o[k]) / d[k];
        if (a > b) { float x = a; a = b; b = x; }
        t0 = fmaxf(t0, a);
        t1 = fminf(t1, b);
        if (t0 > t1) return 0;
    }
    float px = o[0] + d[0] * t0, pz = o[2] + d[2] * t0;
    int ix = (int)floorf((px - m->gx0) / m->cell), iz = (int)floorf((pz - m->gz0) / m->cell);
    ix = ix < 0 ? 0 : ix >= m->gw ? m->gw - 1 : ix;
    iz = iz < 0 ? 0 : iz >= m->gd ? m->gd - 1 : iz;
    int sx = d[0] > 0 ? 1 : -1, sz = d[2] > 0 ? 1 : -1;
    float nx = m->gx0 + (ix + (sx > 0)) * m->cell, nz = m->gz0 + (iz + (sz > 0)) * m->cell;
    float tmx = fabsf(d[0]) > 1e-12f ? (nx - o[0]) / d[0] : 3e38f, tmz = fabsf(d[2]) > 1e-12f ? (nz - o[2]) / d[2] : 3e38f;
    float tdx = fabsf(d[0]) > 1e-12f ? m->cell / fabsf(d[0]) : 3e38f, tdz = fabsf(d[2]) > 1e-12f ? m->cell / fabsf(d[2]) : 3e38f;
    uint32_t q = ++m->query;
    if (!q) { memset(m->stamp, 0, (size_t)m->ntris * 4); q = m->query = 1; }
    int hit = 0;
    for (;;) {
        int c = iz * m->gw + ix;
        for (uint32_t r = m->start[c]; r < m->start[c + 1]; r++) {
            uint32_t tri = m->refs[r];
            if (m->stamp[tri] == q) continue;
            m->stamp[tri] = q;
            float t;
            if (ray_tri(o, d, vp(m, m->idx[tri * 3]), vp(m, m->idx[tri * 3 + 1]), vp(m, m->idx[tri * 3 + 2]), &t) &&
                t <= max && t < *best) {
                *best = t;
                *best_tri = (int)tri;
                hit = 1;
            }
        }
        float exit = fminf(tmx, tmz);
        if (*best <= exit || exit > t1)
            break;
        if (tmx < tmz) { ix += sx; tmx += tdx; }
        else { iz += sz; tmz += tdz; }
        if (ix < 0 || iz < 0 || ix >= m->gw || iz >= m->gd)
            break;
    }
    return hit;
}

/* ------------------------------------------------------------------ world */
void vcpe_col_world(VcpeColWorld *w, VcpeColInstance *inst, int cap_inst, VcpeColCylinder *cyl, int cap_cyl)
{
    memset(w, 0, sizeof *w);
    w->inst = inst;
    w->cap_inst = inst ? cap_inst : 0;
    w->cyl = cyl;
    w->cap_cyl = cyl ? cap_cyl : 0;
    if (inst) memset(inst, 0, sizeof *inst * (size_t)cap_inst);
    if (cyl) memset(cyl, 0, sizeof *cyl * (size_t)cap_cyl);
}

void vcpe_col_ground(VcpeColWorld *w, VcpeColGround ground, VcpeColGroundRay ray, void *user)
{
    w->ground = ground;
    w->ground_ray = ray;
    w->ground_user = user;
}

static void place(VcpeColInstance *in, const float *m)
{
    static const float id[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    memcpy(in->m, m ? m : id, sizeof in->m);
    float s2 = in->m[0] * in->m[0] + in->m[4] * in->m[4] + in->m[8] * in->m[8];
    in->scale = sqrtf(s2);
    /* inverse of s R: R^T / s, i.e. the transpose over s^2 */
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            in->inv[i * 4 + j] = in->m[j * 4 + i] / s2;
    for (int i = 0; i < 3; i++)
        in->inv[i * 4 + 3] = -(in->inv[i * 4] * in->m[3] + in->inv[i * 4 + 1] * in->m[7] + in->inv[i * 4 + 2] * in->m[11]);
    for (int k = 0; k < 3; k++) { in->lo[k] = 3e38f; in->hi[k] = -3e38f; }
    const VcpeColMesh *me = in->mesh;
    for (int c = 0; c < 8; c++) {
        float p[3] = {c & 1 ? me->hi[0] : me->lo[0], c & 2 ? me->hi[1] : me->lo[1], c & 4 ? me->hi[2] : me->lo[2]};
        xf_point(in->m, p, p);
        for (int k = 0; k < 3; k++) { in->lo[k] = fminf(in->lo[k], p[k]); in->hi[k] = fmaxf(in->hi[k], p[k]); }
    }
}

int vcpe_col_add(VcpeColWorld *w, VcpeColMesh *mesh, const float *m, uint32_t user)
{
    if (!w || !mesh || !mesh->ntris)
        return -1;
    for (int i = 0; i < w->cap_inst; i++)
        if (!w->inst[i].used) {
            VcpeColInstance *in = &w->inst[i];
            in->mesh = mesh;
            in->user = user;
            place(in, m);
            if (!(in->scale > 1e-6f)) return -1;
            in->used = 1;
            if (i >= w->ninst) w->ninst = i + 1;
            return i;
        }
    return -1;
}
void vcpe_col_move(VcpeColWorld *w, int id, const float *m)
{
    if (id >= 0 && id < w->ninst && w->inst[id].used) place(&w->inst[id], m);
}
void vcpe_col_remove(VcpeColWorld *w, int id)
{
    if (id >= 0 && id < w->ninst) w->inst[id].used = 0;
}
int vcpe_col_add_cylinder(VcpeColWorld *w, float x, float z, float y0, float y1, float r, uint32_t user)
{
    if (!(r > 0) || !(y1 > y0))
        return -1;
    for (int i = 0; i < w->cap_cyl; i++)
        if (!w->cyl[i].used) {
            w->cyl[i] = (VcpeColCylinder){x, z, y0, y1, r, user, 1};
            if (i >= w->ncyl) w->ncyl = i + 1;
            return i;
        }
    return -1;
}
void vcpe_col_remove_cylinder(VcpeColWorld *w, int id)
{
    if (id >= 0 && id < w->ncyl) w->cyl[id].used = 0;
}

static int ray_box(const float o[3], const float d[3], const float lo[3], const float hi[3], float max)
{
    float t0 = 0, t1 = max;
    for (int k = 0; k < 3; k++) {
        if (fabsf(d[k]) < 1e-12f) {
            if (o[k] < lo[k] || o[k] > hi[k]) return 0;
            continue;
        }
        float a = (lo[k] - o[k]) / d[k], b = (hi[k] - o[k]) / d[k];
        if (a > b) { float x = a; a = b; b = x; }
        t0 = fmaxf(t0, a);
        t1 = fminf(t1, b);
        if (t0 > t1) return 0;
    }
    return 1;
}

static int ray_cylinder(const VcpeColCylinder *c, const float o[3], const float d[3], float max, float *t, float n[3])
{
    float best = 3e38f;
    /* side: |(o + d s) - axis|_xz = r */
    float ox = o[0] - c->x, oz = o[2] - c->z, a = d[0] * d[0] + d[2] * d[2], b = ox * d[0] + oz * d[2];
    float cc = ox * ox + oz * oz - c->r * c->r;
    if (a > 1e-12f && cc > 0) {
        float disc = b * b - a * cc;
        if (disc >= 0) {
            float s = (-b - sqrtf(disc)) / a, y = o[1] + d[1] * s;
            if (s >= 0 && s <= max && y >= c->y0 && y <= c->y1) {
                best = s;
                n[0] = (ox + d[0] * s) / c->r; n[1] = 0; n[2] = (oz + d[2] * s) / c->r;
            }
        }
    }
    /* caps */
    if (fabsf(d[1]) > 1e-12f)
        for (int cap = 0; cap < 2; cap++) {
            float y = cap ? c->y1 : c->y0, s = (y - o[1]) / d[1];
            if (s < 0 || s > max || s >= best) continue;
            float x = ox + d[0] * s, z = oz + d[2] * s;
            if (x * x + z * z <= c->r * c->r) {
                best = s;
                n[0] = 0; n[1] = cap ? 1.0f : -1.0f; n[2] = 0;
            }
        }
    if (cc <= 0 && o[1] >= c->y0 && o[1] <= c->y1) { best = 0; n[0] = 0; n[1] = 1; n[2] = 0; }   /* starts inside */
    if (best > max) return 0;
    *t = best;
    return 1;
}

static int ground_ray(VcpeColWorld *w, const float o[3], const float d[3], float max, float *t, float n[3])
{
    if (!w->ground)
        return 0;
    if (w->ground_ray) {
        if (!w->ground_ray(w->ground_user, o, d, max, t)) return 0;
        float y;
        if (!w->ground(w->ground_user, o[0] + d[0] * *t, o[2] + d[2] * *t, &y, n)) { n[0] = 0; n[1] = 1; n[2] = 0; }
        return 1;
    }
    float horiz = sqrtf(d[0] * d[0] + d[2] * d[2]), y0;
    if (horiz < 1e-6f) {
        /* vertical: the height under the ray decides directly */
        if (!w->ground(w->ground_user, o[0], o[2], &y0, n) || (d[1] < 0 ? o[1] < y0 : o[1] > y0)) return 0;
        *t = (o[1] - y0) / -d[1];
        return *t <= max;
    }
    /* march the heights in half-metre steps, then bisect */
    float step = 0.5f / horiz, prev = 0;
    int have = 0;
    for (float s = 0;; s += step) {
        if (s > max) s = max;
        float p[3] = {o[0] + d[0] * s, o[1] + d[1] * s, o[2] + d[2] * s}, y;
        if (w->ground(w->ground_user, p[0], p[2], &y, n)) {
            if (p[1] <= y) {
                float a = have ? prev : s, b = s;
                if (!have && s > 0) a = 0;
                for (int k = 0; k < 32 && b - a > 1e-5f; k++) {
                    float mid = (a + b) * 0.5f, q[3] = {o[0] + d[0] * mid, o[1] + d[1] * mid, o[2] + d[2] * mid}, my;
                    if (w->ground(w->ground_user, q[0], q[2], &my, n) && q[1] <= my) b = mid;
                    else a = mid;
                }
                if (!have && s == 0) b = 0;
                *t = b;
                w->ground(w->ground_user, o[0] + d[0] * b, o[2] + d[2] * b, &y, n);
                return 1;
            }
            prev = s;
            have = 1;
        } else
            have = 0;
        if (s >= max) break;
    }
    return 0;
}

int vcpe_col_ray(VcpeColWorld *w, const float o[3], const float dir[3], float max, VcpeColHit *hit)
{
    float l = sqrtf(dot3(dir, dir));
    if (!w || !(l > 1e-12f) || !(max > 0))
        return 0;
    float d[3] = {dir[0] / l, dir[1] / l, dir[2] / l}, best = max, n[3];
    VcpeColHit h = {0};
    h.t = max;
    float t;
    if (ground_ray(w, o, d, max, &t, n) && t <= best) {
        best = t;
        h.kind = VCPE_COL_GROUND;
        h.id = -1;
        memcpy(h.normal, n, sizeof n);
    }
    for (int i = 0; i < w->ninst; i++) {
        VcpeColInstance *in = &w->inst[i];
        if (!in->used || !ray_box(o, d, in->lo, in->hi, best)) continue;
        float mo[3], md[3];
        int tri = -1;
        xf_point(in->inv, o, mo);
        xf_vec(in->inv, d, md);
        float bt = best;
        if (mesh_ray(in->mesh, mo, md, best, &bt, &tri)) {
            best = bt;
            h.kind = VCPE_COL_MESH;
            h.id = i;
            h.tri = tri;
            h.user = in->user;
            float wn[3];
            xf_vec(in->m, in->mesh->plane[tri], wn);
            float wl = sqrtf(dot3(wn, wn));
            for (int k = 0; k < 3; k++) h.normal[k] = wn[k] / wl;
            if (dot3(h.normal, d) > 0) for (int k = 0; k < 3; k++) h.normal[k] = -h.normal[k];
        }
    }
    for (int i = 0; i < w->ncyl; i++) {
        const VcpeColCylinder *c = &w->cyl[i];
        if (!c->used) continue;
        if (ray_cylinder(c, o, d, best, &t, n) && t <= best) {
            best = t;
            h.kind = VCPE_COL_CYLINDER;
            h.id = i;
            h.user = c->user;
            memcpy(h.normal, n, sizeof n);
        }
    }
    if (!h.kind)
        return 0;
    h.t = best;
    for (int k = 0; k < 3; k++) h.point[k] = o[k] + d[k] * best;
    if (hit) *hit = h;
    return 1;
}

/* ------------------------------------------------------------------ closest points (Ericson, Real-Time Collision Detection) */
static void closest_tri(const float p[3], const float a[3], const float b[3], const float c[3], float o[3])
{
    float ab[3], ac[3], ap[3], bp[3], cp[3];
    sub3(ab, b, a); sub3(ac, c, a); sub3(ap, p, a);
    float d1 = dot3(ab, ap), d2 = dot3(ac, ap);
    if (d1 <= 0 && d2 <= 0) { memcpy(o, a, 12); return; }
    sub3(bp, p, b);
    float d3 = dot3(ab, bp), d4 = dot3(ac, bp);
    if (d3 >= 0 && d4 <= d3) { memcpy(o, b, 12); return; }
    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) {
        float v = d1 / (d1 - d3);
        for (int k = 0; k < 3; k++) o[k] = a[k] + ab[k] * v;
        return;
    }
    sub3(cp, p, c);
    float d5 = dot3(ab, cp), d6 = dot3(ac, cp);
    if (d6 >= 0 && d5 <= d6) { memcpy(o, c, 12); return; }
    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) {
        float w = d2 / (d2 - d6);
        for (int k = 0; k < 3; k++) o[k] = a[k] + ac[k] * w;
        return;
    }
    float va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
        float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        for (int k = 0; k < 3; k++) o[k] = b[k] + (c[k] - b[k]) * w;
        return;
    }
    float den = 1.0f / (va + vb + vc), v = vb * den, w = vc * den;
    for (int k = 0; k < 3; k++) o[k] = a[k] + ab[k] * v + ac[k] * w;
}

static float closest_seg_seg(const float p1[3], const float q1[3], const float p2[3], const float q2[3], float c1[3], float c2[3])
{
    float d1[3], d2[3], r[3];
    sub3(d1, q1, p1); sub3(d2, q2, p2); sub3(r, p1, p2);
    float a = dot3(d1, d1), e = dot3(d2, d2), f = dot3(d2, r), s, t;
    if (a <= 1e-12f && e <= 1e-12f) s = t = 0;
    else if (a <= 1e-12f) { s = 0; t = clampf(f / e, 0, 1); }
    else {
        float c = dot3(d1, r);
        if (e <= 1e-12f) { t = 0; s = clampf(-c / a, 0, 1); }
        else {
            float b = dot3(d1, d2), den = a * e - b * b;
            s = den > 1e-12f ? clampf((b * f - c * e) / den, 0, 1) : 0;
            t = (b * s + f) / e;
            if (t < 0) { t = 0; s = clampf(-c / a, 0, 1); }
            else if (t > 1) { t = 1; s = clampf((b - c) / a, 0, 1); }
        }
    }
    float dd[3];
    for (int k = 0; k < 3; k++) { c1[k] = p1[k] + d1[k] * s; c2[k] = p2[k] + d2[k] * t; }
    sub3(dd, c1, c2);
    return dot3(dd, dd);
}

/* squared distance between segment p0-p1 and triangle abc (plane: unit normal, offset), closest points out */
static float seg_tri(const float p0[3], const float p1[3], const float *a, const float *b, const float *c, const float pl[4],
                     float qs[3], float qt[3])
{
    float d0 = dot3(pl, p0) - pl[3], d1 = dot3(pl, p1) - pl[3];
    if (d0 * d1 <= 0 && d0 != d1) {
        float t = d0 / (d0 - d1), x[3], on[3];
        for (int k = 0; k < 3; k++) x[k] = p0[k] + (p1[k] - p0[k]) * t;
        closest_tri(x, a, b, c, on);
        float dd[3];
        sub3(dd, x, on);
        if (dot3(dd, dd) < 1e-12f) { memcpy(qs, x, 12); memcpy(qt, x, 12); return 0; }
    }
    float best = 3e38f, s[3], q[3], dd[3];
    const float *ends[2] = {p0, p1};
    for (int e = 0; e < 2; e++) {
        closest_tri(ends[e], a, b, c, q);
        sub3(dd, ends[e], q);
        float d2 = dot3(dd, dd);
        if (d2 < best) { best = d2; memcpy(qs, ends[e], 12); memcpy(qt, q, 12); }
    }
    const float *edges[3][2] = {{a, b}, {b, c}, {c, a}};
    for (int e = 0; e < 3; e++) {
        float d2 = closest_seg_seg(p0, p1, edges[e][0], edges[e][1], s, q);
        if (d2 < best) { best = d2; memcpy(qs, s, 12); memcpy(qt, q, 12); }
    }
    return best;
}

/* ------------------------------------------------------------------ character */
void vcpe_kcc_init(VcpeKcc *k, float radius, float height)
{
    memset(k, 0, sizeof *k);
    k->radius = radius;
    k->height = height > 2 * radius ? height : 2 * radius;
    k->step_up = 0.35f;
    k->step_down = 0.45f;
    k->slope_cos = 0.6947f;      /* 46 degrees */
    k->gravity = 9.81f;
    k->ground_kind = VCPE_COL_NONE;
    k->ground_id = -1;
    k->ground_normal[1] = 1;
}

typedef struct { VcpeColWorld *w; VcpeKcc *k; VcpeColInstance *in; float p0[3], p1[3], r; int contacts; } Push;

static void segment(const VcpeKcc *k, float p0[3], float p1[3])
{
    float lo = k->step_up + k->radius, hi = k->height - k->radius;
    if (hi < lo) hi = lo;
    p0[0] = p1[0] = k->pos[0];
    p0[2] = p1[2] = k->pos[2];
    p0[1] = k->pos[1] + lo;
    p1[1] = k->pos[1] + hi;
}

static int push_tri(void *ctx, VcpeColMesh *m, int tri)
{
    Push *P = ctx;
    const float *pl = m->plane[tri];
    const float *a = vp(m, m->idx[tri * 3]), *b = vp(m, m->idx[tri * 3 + 1]), *c = vp(m, m->idx[tri * 3 + 2]);
    float qs[3], qt[3];
    float d2 = seg_tri(P->p0, P->p1, a, b, c, pl, qs, qt);
    if (d2 >= P->r * P->r)
        return 0;
    float n[3], dist = sqrtf(d2);
    if (dist > 1e-6f) for (int k = 0; k < 3; k++) n[k] = (qs[k] - qt[k]) / dist;
    else {
        /* on the surface: out along the face normal, toward the capsule's middle */
        float mid[3] = {(P->p0[0] + P->p1[0]) * 0.5f, (P->p0[1] + P->p1[1]) * 0.5f, (P->p0[2] + P->p1[2]) * 0.5f};
        float side = dot3(pl, mid) - pl[3] >= 0 ? 1.0f : -1.0f;
        for (int k = 0; k < 3; k++) n[k] = pl[k] * side;
    }
    float depth = P->r - dist + 1e-4f, wn[3], push[3];
    xf_vec(P->in->m, n, wn);
    float wl = sqrtf(dot3(wn, wn));
    for (int k = 0; k < 3; k++) wn[k] /= wl;
    float wdepth = depth * P->in->scale;
    if (wn[1] >= P->k->slope_cos) {
        /* walkable: lift straight up by what resolves the overlap along the normal */
        push[0] = push[2] = 0;
        push[1] = wdepth / wn[1];
    } else {
        for (int k = 0; k < 3; k++) push[k] = wn[k] * wdepth;
        P->contacts++;
        memcpy(P->k->wall_normal, wn, sizeof wn);
    }
    for (int k = 0; k < 3; k++) P->k->pos[k] += push[k];
    float mp[3];
    xf_vec(P->in->inv, push, mp);
    for (int k = 0; k < 3; k++) { P->p0[k] += mp[k]; P->p1[k] += mp[k]; }
    return 0;
}

static int resolve(VcpeColWorld *w, VcpeKcc *k)
{
    int contacts = 0;
    for (int pass = 0; pass < 4; pass++) {
        int before = contacts;
        float moved[3] = {k->pos[0], k->pos[1], k->pos[2]};
        float p0[3], p1[3];
        for (int i = 0; i < w->ninst; i++) {
            VcpeColInstance *in = &w->inst[i];
            if (!in->used) continue;
            segment(k, p0, p1);
            float r = k->radius;
            if (p1[0] + r < in->lo[0] || p0[0] - r > in->hi[0] || p1[2] + r < in->lo[2] || p0[2] - r > in->hi[2] ||
                p1[1] + r < in->lo[1] || p0[1] - r > in->hi[1])
                continue;
            Push P = {w, k, in, {0}, {0}, r / in->scale, 0};
            xf_point(in->inv, p0, P.p0);
            xf_point(in->inv, p1, P.p1);
            float lo[3], hi[3];
            for (int c = 0; c < 3; c++) {
                lo[c] = fminf(P.p0[c], P.p1[c]) - P.r;
                hi[c] = fmaxf(P.p0[c], P.p1[c]) + P.r;
            }
            mesh_box(in->mesh, lo, hi, push_tri, &P);
            contacts += P.contacts;
        }
        segment(k, p0, p1);
        for (int i = 0; i < w->ncyl; i++) {
            const VcpeColCylinder *c = &w->cyl[i];
            if (!c->used || p1[1] + k->radius < c->y0 || p0[1] - k->radius > c->y1) continue;
            float dx = k->pos[0] - c->x, dz = k->pos[2] - c->z, d = sqrtf(dx * dx + dz * dz), lim = c->r + k->radius;
            if (d >= lim) continue;
            if (d < 1e-6f) { dx = 1; dz = 0; d = 1; }
            float depth = lim - d + 1e-4f;
            k->pos[0] += dx / d * depth;
            k->pos[2] += dz / d * depth;
            k->wall_normal[0] = dx / d; k->wall_normal[1] = 0; k->wall_normal[2] = dz / d;
            contacts++;
        }
        float dm[3];
        sub3(dm, k->pos, moved);
        if (contacts == before && dot3(dm, dm) < 1e-10f)
            break;
    }
    return contacts;
}

/* walkable ground between pos.y + step_up and pos.y - reach: under the centre when there is any (so slopes are
 * stood on exactly), else the highest under the four edge points (standing on a ledge) */
static int probe(VcpeColWorld *w, VcpeKcc *k, float reach, float *gy, float gn[3], int *kind, int *id)
{
    float off = k->radius * 0.6f, pts[5][2] = {{0, 0}, {off, 0}, {-off, 0}, {0, off}, {0, -off}};
    float top = k->pos[1] + k->step_up + 0.01f, len = k->step_up + 0.01f + reach;
    int found = 0;
    float best = -3e38f;
    for (int p = 0; p < 5; p++) {
        if (p == 1 && found)
            break;
        float o[3] = {k->pos[0] + pts[p][0], top, k->pos[2] + pts[p][1]}, down[3] = {0, -1, 0}, n[3] = {0, 1, 0}, y;
        if (w->ground && w->ground(w->ground_user, o[0], o[2], &y, n) && y <= top && y >= top - len && n[1] >= k->slope_cos &&
            y > best) {
            best = y; found = 1; memcpy(gn, n, 12); *kind = VCPE_COL_GROUND; *id = -1;
        }
        VcpeColWorld only = *w;
        only.ground = 0;     /* meshes and cylinders only: the ground was handled above */
        VcpeColHit h;
        if (vcpe_col_ray(&only, o, down, len, &h) && h.normal[1] >= k->slope_cos && h.point[1] > best) {
            best = h.point[1]; found = 1; memcpy(gn, h.normal, 12); *kind = h.kind; *id = h.id;
        }
    }
    if (found) *gy = best;
    return found;
}

int vcpe_kcc_move(VcpeColWorld *w, VcpeKcc *k, const float delta[3])
{
    if (!w || !k)
        return 0;
    float len = sqrtf(dot3(delta, delta));
    int n = (int)ceilf(len / (k->radius * 0.5f));
    if (n < 1) n = 1;
    if (n > 64) n = 64;
    int snap = k->grounded && delta[1] <= 0, contacts = 0;
    k->grounded = 0;
    for (int s = 0; s < n; s++) {
        for (int c = 0; c < 3; c++) k->pos[c] += delta[c] / n;
        contacts += resolve(w, k);
        /* never below the ground: walkable ground lifts, steep ground pushes off along its normal */
        float y, gn[3] = {0, 1, 0};
        if (w->ground && w->ground(w->ground_user, k->pos[0], k->pos[2], &y, gn) && k->pos[1] < y) {
            if (gn[1] >= k->slope_cos) k->pos[1] = y;
            else for (int c = 0; c < 3; c++) k->pos[c] += gn[c] * (y - k->pos[1]) * gn[1];
        }
    }
    float gy, gn[3] = {0, 1, 0};
    int kind = 0, id = -1;
    if (probe(w, k, snap ? k->step_down : 0, &gy, gn, &kind, &id) && (gy >= k->pos[1] || snap)) {
        if (delta[1] <= 0 || gy >= k->pos[1]) {
            k->pos[1] = gy;
            k->grounded = 1;
            k->ground_kind = kind;
            k->ground_id = id;
            memcpy(k->ground_normal, gn, sizeof gn);
        }
    }
    if (!k->grounded) {
        k->ground_kind = VCPE_COL_NONE;
        k->ground_id = -1;
    }
    k->contacts = contacts;
    return contacts;
}

void vcpe_kcc_step(VcpeColWorld *w, VcpeKcc *k, const float wish[3], int jump, float jump_speed, float dt)
{
    if (!(dt > 0))
        return;
    if (k->grounded) {
        k->vel[0] = wish[0];
        k->vel[2] = wish[2];
        k->vel[1] = 0;
        if (jump) k->vel[1] = jump_speed;
    } else {
        /* some air control, then gravity */
        float a = dt * 2 > 1 ? 1 : dt * 2;
        k->vel[0] += (wish[0] - k->vel[0]) * a;
        k->vel[2] += (wish[2] - k->vel[2]) * a;
        k->vel[1] -= k->gravity * dt;
    }
    float before = k->pos[1], d[3] = {k->vel[0] * dt, k->vel[1] * dt, k->vel[2] * dt};
    vcpe_kcc_move(w, k, d);
    if (k->grounded)
        k->vel[1] = 0;
    else if (k->vel[1] > 0 && k->pos[1] < before + d[1] - 1e-3f && k->contacts && k->wall_normal[1] < -0.5f)
        k->vel[1] = 0;   /* hit the ceiling */
}
