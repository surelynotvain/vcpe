/* VCPE sky on the GE: rain streaks and snow flakes around the camera (see vcpe_sky.h). */
#include <math.h>
#include "vcpe_3d.h"
#include "vcpe_gfx.h"
#include "vcpe_sky.h"

/* Particles become camera-facing quads in display-list memory (written uncached, so no cache write-back and nothing
 * the GE could still be reading next frame), blended without depth writes after the opaque world. */
int vcpe_sky_draw_precip(const VcpeSky *s, const float cam[3], const float rot[3], int page, int max)
{
    static float pts[1024][3];
    const VcpeWeather *w = &s->now;
    if (max > 1024) max = 1024;
    int n = (int)(w->precip * (float)max + 0.5f);
    if (w->precip_kind == VCPE_PRECIP_NONE || n <= 0)
        return 0;
    vcpe_sky_precip_points(s, cam, pts, n);
    VcpeVtx *v = vcpe_gfx_mem(n * 4 * (int)sizeof(VcpeVtx));
    uint16_t *idx = vcpe_gfx_mem(n * 6 * 2);
    if (!v || !idx)
        return 0;
    const float d2r = 3.14159265f / 180;
    float cy = cosf(rot[1] * d2r), sy = sinf(rot[1] * d2r), cx = cosf(rot[0] * d2r), sx = sinf(rot[0] * d2r);
    float right[3] = {cy, 0, -sy}, up[3] = {sy * sx, cx, cy * sx};
    int snow = w->precip_kind == VCPE_PRECIP_SNOW;
    float a[3], b[3];
    uint32_t color;
    if (snow) {
        for (int k = 0; k < 3; k++) { a[k] = right[k] * 0.035f; b[k] = up[k] * 0.035f; }
        color = 0xd8ffffffu;
    } else {
        /* streaks along the fall (with the wind's slant), a few millimetres wide */
        float f[3] = {w->wind[0], -9.0f, w->wind[1]}, l = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
        for (int k = 0; k < 3; k++) { a[k] = right[k] * 0.008f; b[k] = f[k] / l * 0.22f; }
        color = 0x70d0d0d0u;
    }
    static const float su[4] = {-1, 1, 1, -1}, sv[4] = {1, 1, -1, -1}, tu[4] = {0, 1, 1, 0}, tv[4] = {0, 0, 1, 1};
    for (int i = 0; i < n; i++) {
        for (int c = 0; c < 4; c++) {
            VcpeVtx *q = &v[i * 4 + c];
            q->u = tu[c];
            q->v = tv[c];
            q->color = color;
            q->x = pts[i][0] + a[0] * su[c] + b[0] * sv[c];
            q->y = pts[i][1] + a[1] * su[c] + b[1] * sv[c];
            q->z = pts[i][2] + a[2] * su[c] + b[2] * sv[c];
        }
        uint16_t k = (uint16_t)(i * 4);
        uint16_t *t = &idx[i * 6];
        t[0] = k; t[1] = (uint16_t)(k + 1); t[2] = (uint16_t)(k + 2);
        t[3] = k; t[4] = (uint16_t)(k + 2); t[5] = (uint16_t)(k + 3);
    }
    vcpe_3d_model(0);
    vcpe_3d_material(1, 0);
    vcpe_3d_draw(page, v, idx, n * 6);
    vcpe_3d_material(0, 0x20);
    return n;
}
