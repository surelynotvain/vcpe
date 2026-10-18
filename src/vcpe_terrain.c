/* VCPE terrain: heightfield cells, shared LOD index lists, height queries and relighting (see vcpe_terrain.h). */

/* HOW THIS FILE WORKS
 *
 * Geomipmapping: level l of a cell uses every 2^l-th sample in both directions. All levels index the same vertex
 * array (the full grid), so a level change costs nothing but a different index list, and the lists are the same
 * for every cell of the same size: they are built once per n and shared.
 *
 * Every quad is split along the same diagonal (from its (0, 0) corner to its (1, 1) corner), at every level, and
 * vcpe_terrain_height interpolates on exactly the level-0 triangles, so a character standing on the ground stands on
 * what is drawn up close.
 *
 * Cracks: two neighbouring cells at different levels disagree along their shared edge by at most the coarser level's
 * error. Instead of stitching (which would make index lists depend on the neighbours), every edge has a skirt: a
 * strip hanging straight down from the edge vertices, deeper than the largest error. Seen from above or the side it
 * fills the gap with the edge's own colour.
 *
 * Level errors are measured, not guessed: the largest vertical distance between a sample and the coarse triangle
 * above or below it. The picker turns that into pixels like vcpe_lod (error x px_k / distance to the cell's box),
 * so flat cells go coarse early and cliffs keep their detail.
 *
 * Lighting is per vertex on the CPU: base colour x (ambient + sun colour x max(0, n . sun)). The GE has hardware
 * lights, but they would need a vertex format with normals; recolouring a cell when the sun has moved enough (a few
 * times a minute in game time, spread over frames) is cheaper and keeps the one vertex format of vcpe_3d. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "vcpe_terrain.h"

static int valid_n(int n) { return n >= 2 && n <= VCPE_TERRAIN_MAX_N && !(n & (n - 1)); }
static int levels_for(int n)
{
    int l = 0;
    while ((1 << l) < n && l < VCPE_TERRAIN_LEVELS - 1) l++;
    return l + 1;     /* the coarsest level: one quad per cell, or a step of 32 samples */
}

static int check_blob(const VcpeTerrainBlob *b, uint32_t size)
{
    if (!b || size < sizeof *b || memcmp(b->magic, "VTR1", 4))
        return -1;
    if (((uintptr_t)b & 3) || !valid_n(b->n) || !(b->size > 0) || !(b->step >= 0) || !(b->uv_repeat == b->uv_repeat))
        return -2;
    uint32_t samples = (uint32_t)(b->n + 1) * (b->n + 1);
    if ((b->heights & 1) || (uint64_t)b->heights + samples * 2u > size)
        return -2;
    if (b->colors && ((b->colors & 3) || (uint64_t)b->colors + samples * 4u > size))
        return -2;
    if (b->normals && ((b->normals & 3) || (uint64_t)b->normals + samples * 4u > size))
        return -2;
    return 0;
}

uint32_t vcpe_terrain_cell_bytes(const void *blob, uint32_t size)
{
    const VcpeTerrainBlob *b = blob;
    if (check_blob(b, size))
        return 0;
    uint32_t s = (uint32_t)(b->n + 1), grid = s * s;
    return (grid + 4 * s) * (uint32_t)sizeof(VcpeVtx) + grid * 4u;
}

static float hq(const VcpeTerrainCell *c, int i, int j)
{
    return c->blob->base + c->blob->step * c->h[j * (c->n + 1) + i];
}

/* level-0 style interpolation inside one quad of step s with corner (qi, qj), at fractions u, v */
static float tri_height(const VcpeTerrainCell *c, int qi, int qj, int s, float u, float v)
{
    float a = hq(c, qi, qj), b = hq(c, qi + s, qj), cc = hq(c, qi, qj + s), d = hq(c, qi + s, qj + s);
    return u >= v ? a + (b - a) * u + (d - b) * v : a + (cc - a) * v + (d - cc) * u;
}

/* ------------------------------------------------------------------ shared index lists */
static struct { int n; uint16_t *idx[VCPE_TERRAIN_LEVELS]; int count[VCPE_TERRAIN_LEVELS]; } lists[2];

static int build_lists(int slot, int n)
{
    int s1 = n + 1, grid = s1 * s1, levels = levels_for(n);
    for (int l = 0; l < VCPE_TERRAIN_LEVELS; l++) {
        free(lists[slot].idx[l]);
        lists[slot].idx[l] = 0;
        lists[slot].count[l] = 0;
    }
    lists[slot].n = 0;
    for (int l = 0; l < levels; l++) {
        int st = 1 << l, m = n / st, total = m * m * 6 + 4 * m * 6;
        uint16_t *p = malloc((size_t)total * 2);
        if (!p)
            return -1;
        int k = 0;
        for (int qj = 0; qj < m; qj++)
            for (int qi = 0; qi < m; qi++) {
                int a = qj * st * s1 + qi * st, b = a + st, c = a + st * s1, d = c + st;
                p[k++] = (uint16_t)a; p[k++] = (uint16_t)c; p[k++] = (uint16_t)d;
                p[k++] = (uint16_t)a; p[k++] = (uint16_t)d; p[k++] = (uint16_t)b;
            }
        /* skirts: edge e's skirt vertex for edge sample t is grid + e (n + 1) + t */
        for (int e = 0; e < 4; e++)
            for (int t = 0; t < m; t++) {
                int t0 = t * st, t1 = t0 + st, g0, g1;
                if (e == 0) { g0 = t0; g1 = t1; }                                   /* north, z = 0 */
                else if (e == 1) { g0 = n * s1 + t0; g1 = n * s1 + t1; }            /* south, z = n */
                else if (e == 2) { g0 = t0 * s1; g1 = t1 * s1; }                    /* west, x = 0 */
                else { g0 = t0 * s1 + n; g1 = t1 * s1 + n; }                        /* east, x = n */
                int k0 = grid + e * s1 + t0, k1 = grid + e * s1 + t1;
                p[k++] = (uint16_t)g0; p[k++] = (uint16_t)k0; p[k++] = (uint16_t)k1;
                p[k++] = (uint16_t)g0; p[k++] = (uint16_t)k1; p[k++] = (uint16_t)g1;
            }
        lists[slot].idx[l] = p;
        lists[slot].count[l] = k;
    }
    lists[slot].n = n;
    return 0;
}

const uint16_t *vcpe_terrain_indices(int n, int level, int *count)
{
    *count = 0;
    if (!valid_n(n) || level < 0 || level >= levels_for(n))
        return 0;
    int slot = lists[0].n == n ? 0 : lists[1].n == n ? 1 : -1;
    if (slot < 0) {
        /* worlds use one or two cell sizes; a third replaces the older second */
        slot = !lists[0].n ? 0 : 1;
        if (build_lists(slot, n))
            return 0;
    }
    *count = lists[slot].count[level];
    return lists[slot].idx[level];
}

/* ------------------------------------------------------------------ cells */
static void set_color(VcpeVtx *v, uint32_t c) { v->color = c; }

int vcpe_terrain_cell(VcpeTerrainCell *c, const void *blob, uint32_t size, void *mem, uint32_t cap)
{
    const VcpeTerrainBlob *b = blob;
    memset(c, 0, sizeof *c);
    int r = check_blob(b, size);
    if (r)
        return r;
    uint32_t need = vcpe_terrain_cell_bytes(blob, size);
    if (!mem || ((uintptr_t)mem & 15) || cap < need)
        return -2;
    int n = b->n, s1 = n + 1, grid = s1 * s1;
    int dummy;
    if (!vcpe_terrain_indices(n, 0, &dummy))
        return -2;   /* no memory for the shared lists */
    c->blob = b;
    c->h = (const uint16_t *)((const uint8_t *)b + b->heights);
    c->base_color = b->colors ? (const uint32_t *)((const uint8_t *)b + b->colors) : 0;
    c->verts = mem;
    c->normal = (int8_t (*)[4])((uint8_t *)mem + (size_t)(grid + 4 * s1) * sizeof(VcpeVtx));
    c->n = n;
    c->nverts = grid + 4 * s1;
    c->cx = b->cx;
    c->cz = b->cz;
    c->size = b->size;
    c->quad = b->size / n;
    c->levels = levels_for(n);
    c->level = 0;
    c->ymin = 3.0e38f;
    c->ymax = -3.0e38f;
    float uvk = b->uv_repeat / n;
    for (int j = 0; j < s1; j++)
        for (int i = 0; i < s1; i++) {
            VcpeVtx *v = &c->verts[j * s1 + i];
            float y = hq(c, i, j);
            v->x = i * c->quad;
            v->y = y;
            v->z = j * c->quad;
            v->u = i * uvk;
            v->v = j * uvk;
            uint32_t col = c->base_color ? c->base_color[j * s1 + i] : 0xffffffffu;
            if ((col >> 24) && b->overlay >= 0) c->has_overlay = 1;
            set_color(v, col);
            if (y < c->ymin) c->ymin = y;
            if (y > c->ymax) c->ymax = y;
        }
    /* normals: from the blob (seamless) or central differences, one-sided at the border */
    const int8_t (*bn)[4] = b->normals ? (const int8_t (*)[4])((const uint8_t *)b + b->normals) : 0;
    for (int j = 0; j < s1; j++)
        for (int i = 0; i < s1; i++) {
            int8_t *o = c->normal[j * s1 + i];
            if (bn) {
                memcpy(o, bn[j * s1 + i], 4);
                continue;
            }
            int i0 = i > 0 ? i - 1 : i, i1 = i < n ? i + 1 : i, j0 = j > 0 ? j - 1 : j, j1 = j < n ? j + 1 : j;
            float dx = (hq(c, i1, j) - hq(c, i0, j)) / ((i1 - i0) * c->quad);
            float dz = (hq(c, i, j1) - hq(c, i, j0)) / ((j1 - j0) * c->quad);
            float l = 1.0f / sqrtf(dx * dx + 1 + dz * dz);
            o[0] = (int8_t)lrintf(-dx * l * 127);
            o[1] = (int8_t)lrintf(l * 127);
            o[2] = (int8_t)lrintf(-dz * l * 127);
            o[3] = 0;
        }
    /* level errors */
    float emax = 0;
    for (int l = 1; l < c->levels; l++) {
        int st = 1 << l;
        float e = c->err[l - 1];
        for (int j = 0; j < s1; j++)
            for (int i = 0; i < s1; i++) {
                if (!(i % st) && !(j % st))
                    continue;
                int qi = i / st * st, qj = j / st * st;
                if (qi >= n) qi = n - st;
                if (qj >= n) qj = n - st;
                float d = fabsf(hq(c, i, j) - tri_height(c, qi, qj, st, (float)(i - qi) / st, (float)(j - qj) / st));
                if (d > e) e = d;
            }
        c->err[l] = e;
        if (e > emax) emax = e;
    }
    c->skirt = 2 * emax + 0.1f * c->quad + 0.05f;
    /* skirt vertices: copies of the edge vertices, lowered */
    for (int e = 0; e < 4; e++)
        for (int t = 0; t < s1; t++) {
            int g = e == 0 ? t : e == 1 ? n * s1 + t : e == 2 ? t * s1 : t * s1 + n;
            VcpeVtx *v = &c->verts[grid + e * s1 + t];
            *v = c->verts[g];
            v->y -= c->skirt;
        }
    c->ymin -= c->skirt;
    c->dirty = 1;
    c->light_serial = 0;
    return 0;
}

int vcpe_terrain_height(const VcpeTerrainCell *c, float x, float z, float *y, float normal[3])
{
    if (!c || !c->blob)
        return 0;
    float lx = (x - c->cx * c->size) / c->quad, lz = (z - c->cz * c->size) / c->quad;
    if (!(lx >= 0 && lz >= 0 && lx <= c->n && lz <= c->n))
        return 0;
    int qi = (int)lx, qj = (int)lz;
    if (qi >= c->n) qi = c->n - 1;
    if (qj >= c->n) qj = c->n - 1;
    float u = lx - qi, v = lz - qj;
    if (y)
        *y = tri_height(c, qi, qj, 1, u, v);
    if (normal) {
        float a = hq(c, qi, qj), b = hq(c, qi + 1, qj), cc = hq(c, qi, qj + 1), d = hq(c, qi + 1, qj + 1);
        float dx, dz;
        if (u >= v) { dx = b - a; dz = d - b; }
        else { dx = d - cc; dz = cc - a; }
        dx /= c->quad;
        dz /= c->quad;
        float l = 1.0f / sqrtf(dx * dx + 1 + dz * dz);
        normal[0] = -dx * l;
        normal[1] = l;
        normal[2] = -dz * l;
    }
    return 1;
}

int vcpe_terrain_height_any(VcpeTerrainCell *const *cells, int count, float x, float z, float *y, float normal[3])
{
    for (int i = 0; i < count; i++)
        if (cells[i] && vcpe_terrain_height(cells[i], x, z, y, normal))
            return 1;
    return 0;
}

int vcpe_terrain_ray(const VcpeTerrainCell *c, const float o[3], const float d[3], float max_dist, float *hit_t)
{
    if (!c || !c->blob || !(max_dist > 0))
        return 0;
    float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len < 1e-9f)
        return 0;
    float step = c->quad * 0.5f / len, prev_t = 0;
    int have_prev = 0;
    for (float t = 0;; t += step) {
        if (t > max_dist / len) t = max_dist / len;
        float p[3] = {o[0] + d[0] * t, o[1] + d[1] * t, o[2] + d[2] * t}, gy;
        if (vcpe_terrain_height(c, p[0], p[2], &gy, 0)) {
            float gap = p[1] - gy;
            if (gap <= 0) {
                if (!have_prev) {
                    if (hit_t) *hit_t = t * len;   /* starts below the ground */
                    return 1;
                }
                float a = prev_t, b = t;
                for (int k = 0; k < 12; k++) {
                    float m = (a + b) * 0.5f, q[3] = {o[0] + d[0] * m, o[1] + d[1] * m, o[2] + d[2] * m}, my;
                    if (vcpe_terrain_height(c, q[0], q[2], &my, 0) && q[1] - my <= 0) b = m;
                    else a = m;
                }
                if (hit_t) *hit_t = b * len;
                return 1;
            }
            prev_t = t;
            have_prev = 1;
        } else
            have_prev = 0;
        if (t >= max_dist / len)
            break;
    }
    return 0;
}

/* ------------------------------------------------------------------ lighting */
typedef struct { int sun[3], amb[3], dir[3]; } Light;
static void light_prep(const VcpeTerrainLight *l, Light *o)
{
    float s[3] = {l->sun[0], l->sun[1], l->sun[2]}, n = sqrtf(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    if (n < 1e-9f) { s[0] = 0; s[1] = 1; s[2] = 0; n = 1; }
    for (int k = 0; k < 3; k++) {
        o->dir[k] = (int)lrintf(s[k] / n * 256);           /* x 256; normals are x 127 */
        o->sun[k] = (int)(l->sun_color >> (8 * k) & 255);
        o->amb[k] = (int)(l->ambient >> (8 * k) & 255);
    }
}
static uint32_t lit(uint32_t base, const int8_t *nrm, const Light *L)
{
    int d = nrm[0] * L->dir[0] + nrm[1] * L->dir[1] + nrm[2] * L->dir[2];   /* x 127 x 256 */
    if (d < 0) d = 0;
    uint32_t out = base & 0xff000000u;
    for (int k = 0; k < 3; k++) {
        int light = L->amb[k] + (int)(((int64_t)L->sun[k] * d) / (127 * 256));
        if (light > 255) light = 255;
        int c = (int)(base >> (8 * k) & 255);
        out |= (uint32_t)((c * light + 127) / 255) << (8 * k);
    }
    return out;
}

void vcpe_terrain_relight(VcpeTerrainCell *c, const VcpeTerrainLight *l)
{
    if (!c || !c->blob || !l)
        return;
    Light L;
    light_prep(l, &L);
    int s1 = c->n + 1, grid = s1 * s1;
    for (int k = 0; k < grid; k++)
        c->verts[k].color = lit(c->base_color ? c->base_color[k] : 0xffffffffu, c->normal[k], &L);
    for (int e = 0; e < 4; e++)
        for (int t = 0; t < s1; t++) {
            int g = e == 0 ? t : e == 1 ? c->n * s1 + t : e == 2 ? t * s1 : t * s1 + c->n;
            c->verts[grid + e * s1 + t].color = c->verts[g].color;
        }
    c->light_serial = l->serial;
    c->dirty = 1;
}

int vcpe_terrain_relight_some(VcpeTerrainCell *const *cells, int count, const VcpeTerrainLight *l, int max_vertices)
{
    int done = 0, used = 0;
    for (int i = 0; i < count; i++) {
        VcpeTerrainCell *c = cells[i];
        if (!c || !c->blob || c->light_serial == l->serial)
            continue;
        if (done && used + c->nverts > max_vertices)
            break;
        vcpe_terrain_relight(c, l);
        used += c->nverts;
        done++;
    }
    return done;
}

/* ------------------------------------------------------------------ LOD */
void vcpe_terrain_bounds(const VcpeTerrainCell *c, const float o[3], float center[3], float extent[3])
{
    float x0 = c->cx * c->size - o[0], z0 = c->cz * c->size - o[2];
    center[0] = x0 + c->size * 0.5f;
    center[1] = (c->ymin + c->ymax) * 0.5f - o[1];
    center[2] = z0 + c->size * 0.5f;
    extent[0] = extent[2] = c->size * 0.5f;
    extent[1] = (c->ymax - c->ymin) * 0.5f;
}

int vcpe_terrain_pick(const VcpeTerrainCell *c, const VcpeLodView *v, const float o[3], int prev)
{
    float ctr[3], ext[3], d2 = 0;
    vcpe_terrain_bounds(c, o, ctr, ext);
    for (int k = 0; k < 3; k++) {
        float d = fabsf(v->pos[k] - ctr[k]) - ext[k];
        if (d > 0) d2 += d * d;
    }
    float dist = sqrtf(d2);
    if (dist < 1e-3f)
        return 0;
    int best = 0;
    for (int l = 1; l < c->levels; l++) {
        float px = c->err[l] * v->px_k / dist, lim = v->thresh_px * (l > prev ? v->down_k : 1.0f);
        if (px <= lim) best = l;
        else break;
    }
    return best;
}

/* ------------------------------------------------------------------ far terrain */
uint32_t vcpe_terrain_far_bytes(int w, int d)
{
    if (w < 1 || d < 1 || (w + 1) * (d + 1) > 65535)
        return 0;
    uint32_t verts = (uint32_t)((w + 1) * (d + 1)) * (uint32_t)sizeof(VcpeVtx);
    return verts + (uint32_t)(w * d * 6) * 2u;
}

int vcpe_terrain_far_init(VcpeTerrainFar *f, const uint16_t *h, int w, int d, float x0, float z0, float quad,
                          float base, float step, int16_t page, uint32_t color, void *mem)
{
    memset(f, 0, sizeof *f);
    if (!h || !mem || ((uintptr_t)mem & 15) || !vcpe_terrain_far_bytes(w, d) || !(quad > 0))
        return -1;
    f->h = h;
    f->w = w;
    f->d = d;
    f->x0 = x0;
    f->z0 = z0;
    f->quad = quad;
    f->base = base;
    f->step = step;
    f->page = page;
    f->color = color;
    f->verts = mem;
    f->idx = (uint16_t *)((uint8_t *)mem + (size_t)(w + 1) * (d + 1) * sizeof(VcpeVtx));
    for (int j = 0; j <= d; j++)
        for (int i = 0; i <= w; i++) {
            VcpeVtx *v = &f->verts[j * (w + 1) + i];
            v->x = i * quad;
            v->z = j * quad;
            v->y = base + step * h[j * (w + 1) + i];
            v->u = (float)i / w;
            v->v = (float)j / d;
            v->color = color;
        }
    f->vertex_limit = (w + 1) * (d + 1);
    vcpe_terrain_far_cut(f, 0, 0, 0, 0);
    return 0;
}

int vcpe_terrain_far_cut(VcpeTerrainFar *f, float hx0, float hz0, float hx1, float hz1)
{
    int k = 0, w1 = f->w + 1;
    for (int j = 0; j < f->d; j++)
        for (int i = 0; i < f->w; i++) {
            float cx = f->x0 + (i + 0.5f) * f->quad, cz = f->z0 + (j + 0.5f) * f->quad;
            if (hx1 > hx0 && cx > hx0 && cx < hx1 && cz > hz0 && cz < hz1)
                continue;
            int a = j * w1 + i, b = a + 1, c = a + w1, d = c + 1;
            f->idx[k++] = (uint16_t)a; f->idx[k++] = (uint16_t)c; f->idx[k++] = (uint16_t)d;
            f->idx[k++] = (uint16_t)a; f->idx[k++] = (uint16_t)d; f->idx[k++] = (uint16_t)b;
        }
    f->nidx = k;
    f->cut_changed = 1;
    f->dirty = 1;
    return k / 3;
}

void vcpe_terrain_far_relight(VcpeTerrainFar *f, const VcpeTerrainLight *l)
{
    Light L;
    light_prep(l, &L);
    int w1 = f->w + 1;
    for (int j = 0; j <= f->d; j++)
        for (int i = 0; i <= f->w; i++) {
            /* normals from the coarse heights (one-sided at the border) */
            int i0 = i > 0 ? i - 1 : i, i1 = i < f->w ? i + 1 : i, j0 = j > 0 ? j - 1 : j, j1 = j < f->d ? j + 1 : j;
            float dx = (f->h[j * w1 + i1] - (float)f->h[j * w1 + i0]) * f->step / ((i1 - i0) * f->quad);
            float dz = (f->h[j1 * w1 + i] - (float)f->h[j0 * w1 + i]) * f->step / ((j1 - j0) * f->quad);
            float il = 1.0f / sqrtf(dx * dx + 1 + dz * dz);
            int8_t n[4] = {(int8_t)lrintf(-dx * il * 127), (int8_t)lrintf(il * 127), (int8_t)lrintf(-dz * il * 127), 0};
            f->verts[j * w1 + i].color = lit(f->color, n, &L);
        }
    f->light_serial = l->serial;
    f->dirty = 1;
}
