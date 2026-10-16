/* VCPE LOD meshes: reading VLD1 blobs and picking a detail level (see vcpe_lod.h). */

/* HOW THIS FILE WORKS
 *
 * tools/vcpe_lod.py does the heavy part offline: it joins triangles (edge collapses), measures how far every
 * level strays from the full mesh (err, in model units) and orders the vertices so each level is a prefix.
 *
 * At runtime only one question is left: how big does that error look from here? An error e at distance d covers
 * about e x px_k / d pixels, where px_k = (screen height / 2) / tan(FOV / 2) is the projection's scale. The
 * distance is taken to the nearest point of the bounding sphere, so a big object seen up close is never
 * underestimated. The coarsest level under the threshold wins.
 *
 * Hysteresis: going coarser needs the error to drop below thresh x down_k, going finer happens as soon as the
 * error passes thresh. Without it a mesh sitting at a boundary distance would switch every frame.
 *
 * No PSP headers here: the module is plain C, tested on the host (tests/host/lod_test.c). */
#include <math.h>
#include <string.h>
#include "vcpe_lod.h"
float vcpe_adapt_lod_factor(void) __attribute__((weak));

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static float rdf(const uint8_t *p) { float f; memcpy(&f, p, 4); return f; }

int vcpe_lod_open(VcpeLodMesh *m, const void *blob, int size)
{
    const uint8_t *b = blob;
    memset(m, 0, sizeof *m);
    if (!b || size < 32 || memcmp(b, "VLD1", 4))
        return -1;
    int nv = rd16(b + 4), nl = rd16(b + 6);
    int vert_off = (32 + 16 * nl + 15) & ~15;
    if (nl < 1 || nl > VCPE_LOD_MAX || nv < 1 || vert_off + nv * 24 > size || ((uintptr_t)b & 3))
        return -2;
    const VcpeLodLevel *lv = (const VcpeLodLevel *)(b + 32);
    for (int l = 0; l < nl; l++) {
        if (lv[l].nverts > nv || lv[l].nidx % 3 || (lv[l].idx_off & 1) || lv[l].idx_off + lv[l].nidx * 2u > (unsigned)size)
            return -2;
        if (l && (lv[l].nidx > lv[l - 1].nidx || lv[l].err < lv[l - 1].err))
            return -2;   /* levels go from fine to coarse */
    }
    m->base = b;
    m->lv = lv;
    m->verts = b + vert_off;
    m->nverts = nv;
    m->nlevels = nl;
    for (int i = 0; i < 3; i++)
        m->centre[i] = rdf(b + 8 + 4 * i);
    m->radius = rdf(b + 20);
    return 0;
}

void vcpe_lod_view(VcpeLodView *v, const float cam_pos[3], float fov_deg, int screen_h, float thresh_px)
{
    for (int i = 0; i < 3; i++)
        v->pos[i] = cam_pos[i];
    v->px_k = (float)screen_h * 0.5f / tanf(fov_deg * 0.5f * 3.14159265f / 180.0f);
    v->thresh_px = thresh_px > 0 ? thresh_px : 1.0f;
    v->down_k = 0.75f;
}

/* pixels per model unit of error at the object's nearest point; huge when the camera is inside the sphere */
static float px_per_unit(const VcpeLodMesh *m, const VcpeLodView *v, const float c[3], float scale)
{
    float dx = c[0] - v->pos[0], dy = c[1] - v->pos[1], dz = c[2] - v->pos[2];
    float d = sqrtf(dx * dx + dy * dy + dz * dz) - m->radius * scale;
    if (d < 1e-3f)
        return 1e30f;
    return scale * v->px_k / d;
}

float vcpe_lod_error_px(const VcpeLodMesh *m, const VcpeLodView *v, const float c[3], float scale, int level)
{
    if (level < 0 || level >= m->nlevels)
        return 0;
    return m->lv[level].err * px_per_unit(m, v, c, scale);
}

int vcpe_lod_pick(const VcpeLodMesh *m, const VcpeLodView *v, const float c[3], float scale, int prev)
{
    float k = px_per_unit(m, v, c, scale);
    float adapt = vcpe_adapt_lod_factor ? vcpe_adapt_lod_factor() : 1.0f;
    int pick = 0;
    for (int l = m->nlevels - 1; l > 0; l--) {
        float t = v->thresh_px * adapt;
        if (prev >= 0 && l > prev)
            t *= v->down_k;   /* coarser than now: must be well under the threshold */
        if (m->lv[l].err * k <= t) {
            pick = l;
            break;
        }
    }
    return pick;
}
