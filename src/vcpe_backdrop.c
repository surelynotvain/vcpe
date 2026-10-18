#include "vcpe_backdrop.h"
#include "vcpe_3d.h"
#include "vcpe_gfx.h"
#include "vcpe_math.h"
#include <pspkernel.h>
#include <string.h>

#define SKY_SEG 64
#define SKY_RINGS 5
#define RIDGE_SEG 96
static VcpeVtx sky_v[SKY_SEG * SKY_RINGS + 1] __attribute__((aligned(16)));
static VcpeVtx ridge_v[2][(RIDGE_SEG + 1) * 3] __attribute__((aligned(16)));
static uint16_t sky_i[SKY_SEG * (SKY_RINGS - 1) * 6 + SKY_SEG * 3], ridge_i[RIDGE_SEG * 12];
static int sky_n, layers;
static float ridge_base[2];   /* height above which a ridge vertex counts as snow cap, per layer (scaled) */
static VcpeVtx sun_v[4] __attribute__((aligned(16)));
static const uint16_t sun_i[6] __attribute__((aligned(16))) = {0, 1, 2, 0, 2, 3};
static uint32_t sun_color;
static int sun_page = -1;

/* the CPU wrote these through the data cache; the GE reads RAM */
static void written(const void *p, unsigned n) { sceKernelDcacheWritebackRange(p, n); }

static uint32_t mix(uint32_t a, uint32_t b, float t)
{
    uint32_t out = 0xff000000u;
    for (int k = 0; k < 24; k += 8) {
        float x = ((a >> k) & 255) * (1 - t) + ((b >> k) & 255) * t;
        out |= (uint32_t)(x + .5f) << k;
    }
    return out;
}

void vcpe_backdrop_defaults(VcpeBackdrop *b)
{
    b->haze = 0xffb9c1c4u; b->zenith = 0xffb7a595u;
    b->rock = 0xff6f7477u; b->snow = 0xffe6eaebu;
    b->layers = 2; b->height = 1; b->seed = 0;
}

void vcpe_backdrop_build(const VcpeBackdrop *b)
{
    static const float elev[SKY_RINGS] = {-30, 0, 10, 30, 60};
    int n = 0;
    for (int r = 0; r < SKY_RINGS; r++)
        for (int i = 0; i < SKY_SEG; i++) {
            float a = i * 360.0f / SKY_SEG, ce = vcpe_cos_deg(elev[r]), t = elev[r] <= 0 ? 0 : elev[r] / 90;
            VcpeVtx *v = &sky_v[r * SKY_SEG + i];
            v->u = v->v = 0;
            v->color = mix(b->haze, b->zenith, vcpe_sqrt(t));
            v->x = vcpe_sin_deg(a) * ce * 150; v->y = vcpe_sin_deg(elev[r]) * 150; v->z = vcpe_cos_deg(a) * ce * 150;
        }
    VcpeVtx *top = &sky_v[SKY_SEG * SKY_RINGS];
    top->u = top->v = 0; top->color = b->zenith; top->x = top->z = 0; top->y = 150;
    for (int r = 0; r < SKY_RINGS - 1; r++)
        for (int i = 0; i < SKY_SEG; i++) {
            int a = r * SKY_SEG + i, c = r * SKY_SEG + (i + 1) % SKY_SEG;
            uint16_t q[6] = {a, c, a + SKY_SEG, c, c + SKY_SEG, a + SKY_SEG};
            memcpy(sky_i + n, q, sizeof q); n += 6;
        }
    for (int i = 0; i < SKY_SEG; i++) {
        int r = (SKY_RINGS - 1) * SKY_SEG;
        sky_i[n++] = r + i; sky_i[n++] = r + (i + 1) % SKY_SEG; sky_i[n++] = SKY_SEG * SKY_RINGS;
    }
    sky_n = n;
    layers = b->layers < 0 ? 0 : b->layers > 2 ? 2 : b->layers;
    float seed = (float)(b->seed % 360);
    for (int layer = 0; layer < 2; layer++) {
        float radius = layer ? 120 : 140, haze = layer ? .45f : .68f;
        uint32_t rock = mix(b->rock, b->haze, haze), snow = mix(b->snow, b->haze, haze * .6f);
        for (int i = 0; i <= RIDGE_SEG; i++) {
            float a = i * 360.0f / RIDGE_SEG + layer * 17;
            /* Deterministic ridged noise; the closer layer is lower but sharper. */
            float h = 0, f = 1, amp = 1;
            for (int o = 0; o < 4; o++) {
                float w = 1 - vcpe_absf(vcpe_sin_deg(a * f * (2 + layer) + o * 71 + layer * 29 + seed));
                h += w * w * amp; f *= 2.13f; amp *= .5f;
            }
            h = ((layer ? 5 : 9) + h * (layer ? 9 : 12)) * b->height;
            float sx = vcpe_sin_deg(a) * radius, sz = vcpe_cos_deg(a) * radius;
            float cap = vcpe_clampf((h - (layer ? 11 : 15) * b->height) / 6, 0, 1);
            VcpeVtx *v = &ridge_v[layer][i * 3];
            for (int k = 0; k < 3; k++) { v[k].u = v[k].v = 0; v[k].x = sx; v[k].z = sz; }
            v[0].y = -12; v[0].color = b->haze;
            v[1].y = h * .62f; v[1].color = rock;
            v[2].y = h; v[2].color = mix(rock, snow, .35f + .65f * cap);
        }
    }
    n = 0;
    for (int i = 0; i < RIDGE_SEG; i++)
        for (int k = 0; k < 2; k++) {
            int a = i * 3 + k, c = (i + 1) * 3 + k;
            uint16_t q[6] = {a, c, a + 1, c, c + 1, a + 1};
            memcpy(ridge_i + n, q, sizeof q); n += 6;
        }
    ridge_base[0] = 15 * b->height;
    ridge_base[1] = 11 * b->height;
    written(sky_v, sizeof sky_v);
    written(sky_i, sizeof sky_i);
    written(ridge_v, sizeof ridge_v);
    written(ridge_i, sizeof ridge_i);
}

/* the colours vcpe_backdrop_build gives, from the geometry already built (the cap from each peak's height) */
void vcpe_backdrop_recolor(const VcpeBackdrop *b)
{
    static const float elev[SKY_RINGS] = {-30, 0, 10, 30, 60};
    if (!sky_n)
        return;
    for (int r = 0; r < SKY_RINGS; r++) {
        float t = elev[r] <= 0 ? 0 : elev[r] / 90;
        uint32_t c = mix(b->haze, b->zenith, vcpe_sqrt(t));
        for (int i = 0; i < SKY_SEG; i++)
            sky_v[r * SKY_SEG + i].color = c;
    }
    sky_v[SKY_SEG * SKY_RINGS].color = b->zenith;
    for (int layer = 0; layer < 2; layer++) {
        float haze = layer ? .45f : .68f;
        uint32_t rock = mix(b->rock, b->haze, haze), snow = mix(b->snow, b->haze, haze * .6f);
        for (int i = 0; i <= RIDGE_SEG; i++) {
            VcpeVtx *v = &ridge_v[layer][i * 3];
            float cap = vcpe_clampf((v[2].y - ridge_base[layer]) / 6, 0, 1);
            v[0].color = b->haze;
            v[1].color = rock;
            v[2].color = mix(rock, snow, .35f + .65f * cap);
        }
    }
    written(sky_v, sizeof sky_v);
    written(ridge_v, sizeof ridge_v);
}

void vcpe_backdrop_sun(const float dir[3], float size_deg, uint32_t color, int page)
{
    sun_color = color;
    sun_page = page;
    if (!(color >> 24) || !dir)
        return;
    float l = vcpe_sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (l < 1e-6f) { sun_color = 0; return; }
    float d[3] = {dir[0] / l, dir[1] / l, dir[2] / l}, up[3] = {0, 1, 0};
    if (vcpe_absf(d[1]) > 0.99f) { up[0] = 0; up[1] = 0; up[2] = 1; }
    /* a square facing the camera at the dome's distance, between the dome (150) and the ridges (120..140) */
    float r[3] = {up[1] * d[2] - up[2] * d[1], up[2] * d[0] - up[0] * d[2], up[0] * d[1] - up[1] * d[0]};
    float rl = vcpe_sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    for (int k = 0; k < 3; k++) r[k] /= rl;
    float u[3] = {d[1] * r[2] - d[2] * r[1], d[2] * r[0] - d[0] * r[2], d[0] * r[1] - d[1] * r[0]};
    float dist = 145, half = dist * vcpe_sin_deg(size_deg * 0.5f);
    static const float cu[4] = {0, 1, 1, 0}, cv[4] = {0, 0, 1, 1}, sx[4] = {-1, 1, 1, -1}, sy[4] = {1, 1, -1, -1};
    for (int c = 0; c < 4; c++) {
        VcpeVtx *v = &sun_v[c];
        v->u = cu[c];
        v->v = cv[c];
        v->color = color;
        v->x = d[0] * dist + (r[0] * sx[c] + u[0] * sy[c]) * half;
        v->y = d[1] * dist + (r[1] * sx[c] + u[1] * sy[c]) * half;
        v->z = d[2] * dist + (r[2] * sx[c] + u[2] * sy[c]) * half;
    }
    written(sun_v, sizeof sun_v);
}

void vcpe_backdrop_draw(const float rot[3], float fov)
{
    static const float origin[3] = {0, 0, 0};
    if (!sky_n)
        return;
    vcpe_3d_fog(0, 0, 0);
    vcpe_3d_begin(origin, rot, fov, 1, 400);
    vcpe_3d_draw(-1, sky_v, sky_i, sky_n);
    if (sun_color >> 24) {
        vcpe_3d_material(1, 0);
        vcpe_3d_draw(sun_page, sun_v, sun_i, 6);
        vcpe_3d_material(0, 0x20);
    }
    for (int l = 0; l < layers; l++)
        vcpe_3d_draw(-1, ridge_v[l], ridge_i, RIDGE_SEG * 12);
    vcpe_3d_end();
}
