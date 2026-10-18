/* VCPE sky: day cycle, weather blending and precipitation positions (see vcpe_sky.h). */

/* HOW THIS FILE WORKS
 *
 * The sun moves on a circle: rising at 6:00 toward `yaw`, highest at 12:00 (`tilt` degrees from the zenith, toward
 * +Z), setting at 18:00. Its elevation drives everything else through two smooth weights: day (0 deep night, 1
 * full day, ramping across the horizon) and twilight (largest with the sun on the horizon). Clear-sky colours mix
 * night, day and twilight palettes by those weights; then the weather greys them (overcast takes the colour toward
 * its own luminance and dims the sun), multiplies them by its tint and sets the fog distances. At night the light
 * comes from the moon, opposite the sun, dim and blue.
 *
 * Weather changes are a blend from what is showing to the new weather (smoothstepped), so a change requested in the
 * middle of another one starts from the current mix and never jumps. Rain and snow swap kind at the halfway point,
 * each fading out and in.
 *
 * Terrain relighting is expensive, so `serial` only changes when the light moved more than 1.5 degrees or a light
 * colour channel by more than 4: at the default 72-minute day that is every few seconds of real time.
 *
 * Precipitation is stateless: particle i's position is a hash of i plus its fall and drift over `time`, wrapped
 * into a box around the camera. Nothing is stored, a pause freezes it, and the same frame always draws the same. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "vcpe_sky.h"

#define PI_F 3.14159265f

static float smooth(float e0, float e1, float x)
{
    float t = (x - e0) / (e1 - e0);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return t * t * (3 - 2 * t);
}
static float lerpf(float a, float b, float t) { return a + (b - a) * t; }
/* ABGR colours as float r, g, b 0..255 */
static void unpack(uint32_t c, float o[3]) { o[0] = (float)(c & 255); o[1] = (float)(c >> 8 & 255); o[2] = (float)(c >> 16 & 255); }
static uint32_t pack(const float c[3])
{
    uint32_t out = 0xff000000u;
    for (int k = 0; k < 3; k++) {
        float v = c[k] < 0 ? 0 : c[k] > 255 ? 255 : c[k];
        out |= (uint32_t)(v + 0.5f) << (8 * k);
    }
    return out;
}
static void mix3(float o[3], const float a[3], const float b[3], float t) { for (int k = 0; k < 3; k++) o[k] = lerpf(a[k], b[k], t); }

void vcpe_weather_preset(VcpeWeather *w, int preset)
{
    /* overcast, fog near, far, precipitation, kind, wind x, z, tint */
    static const struct { float o, fn, ff, p; int kind; float wx, wz; uint32_t tint; } t[VCPE_WEATHER_COUNT] = {
        {0.0f, 60, 420, 0.0f, VCPE_PRECIP_NONE, 1.0f, 0.5f, 0xffffffffu},   /* clear */
        {0.4f, 50, 330, 0.0f, VCPE_PRECIP_NONE, 2.0f, 1.0f, 0xffffffffu},   /* cloudy */
        {0.8f, 40, 250, 0.0f, VCPE_PRECIP_NONE, 2.0f, 1.0f, 0xfff4f4f4u},   /* overcast */
        {0.85f, 30, 180, 0.7f, VCPE_PRECIP_RAIN, 2.0f, 1.0f, 0xffe8e8e8u},  /* rain */
        {1.0f, 20, 120, 1.0f, VCPE_PRECIP_RAIN, 6.0f, 3.0f, 0xffd8d4d4u},   /* storm */
        {0.75f, 25, 160, 0.6f, VCPE_PRECIP_SNOW, 1.0f, 0.5f, 0xfffaf8f8u},  /* snow */
        {1.0f, 8, 60, 1.0f, VCPE_PRECIP_SNOW, 7.0f, 2.0f, 0xfff8f4f0u},     /* blizzard */
        {0.6f, 4, 45, 0.0f, VCPE_PRECIP_NONE, 0.3f, 0.2f, 0xfff0f0f0u},     /* fog */
    };
    int p = preset < 0 || preset >= VCPE_WEATHER_COUNT ? 0 : preset;
    w->overcast = t[p].o;
    w->fog_near = t[p].fn;
    w->fog_far = t[p].ff;
    w->precip = t[p].p;
    w->precip_kind = t[p].kind;
    w->wind[0] = t[p].wx;
    w->wind[1] = t[p].wz;
    w->tint = t[p].tint;
}

static void weather_mix(VcpeWeather *o, const VcpeWeather *a, const VcpeWeather *b, float t)
{
    o->overcast = lerpf(a->overcast, b->overcast, t);
    o->fog_near = lerpf(a->fog_near, b->fog_near, t);
    o->fog_far = lerpf(a->fog_far, b->fog_far, t);
    o->wind[0] = lerpf(a->wind[0], b->wind[0], t);
    o->wind[1] = lerpf(a->wind[1], b->wind[1], t);
    float ta[3], tb[3], tc[3];
    unpack(a->tint, ta);
    unpack(b->tint, tb);
    mix3(tc, ta, tb, t);
    o->tint = pack(tc);
    if (a->precip_kind == b->precip_kind || !a->precip || !b->precip) {
        o->precip_kind = b->precip ? b->precip_kind : a->precip_kind;
        o->precip = lerpf(a->precip, b->precip, t);
        if (!b->precip && t >= 1) o->precip_kind = VCPE_PRECIP_NONE;
    } else if (t < 0.5f) {
        o->precip_kind = a->precip_kind;
        o->precip = a->precip * (1 - 2 * t);
    } else {
        o->precip_kind = b->precip_kind;
        o->precip = b->precip * (2 * t - 1);
    }
}

static void compute(VcpeSky *s)
{
    const float d2r = PI_F / 180;
    float th = (s->hour - 6) / 12 * PI_F, tilt = s->tilt * d2r, yaw = s->yaw * d2r;
    float px = cosf(th), py = sinf(th) * cosf(tilt), pz = sinf(th) * sinf(tilt);
    float sun[3] = {px * cosf(yaw) + pz * sinf(yaw), py, -px * sinf(yaw) + pz * cosf(yaw)};
    memcpy(s->light_dir_sun, sun, sizeof sun);
    float e = sun[1];
    s->sun_elev = e;
    float day = smooth(-0.12f, 0.22f, e), twi = 1 - smooth(0, 0.28f, fabsf(e + 0.02f));
    weather_mix(&s->now, &s->from, &s->to, smooth(0, 1, s->blend));
    const VcpeWeather *w = &s->now;

    static const float night_zen[3] = {10, 14, 30}, day_zen[3] = {62, 118, 196}, twi_zen[3] = {70, 64, 110};
    static const float night_hor[3] = {22, 28, 44}, day_hor[3] = {182, 200, 214}, twi_hor[3] = {238, 142, 84};
    static const float night_amb[3] = {26, 30, 44}, day_amb[3] = {92, 100, 112};
    static const float moon[3] = {70, 82, 112}, noon[3] = {255, 246, 228}, low[3] = {255, 150, 86};
    float zen[3], hor[3], amb[3], sc[3];
    mix3(zen, night_zen, day_zen, day);
    mix3(zen, zen, twi_zen, twi * 0.35f);
    mix3(hor, night_hor, day_hor, day);
    mix3(hor, hor, twi_hor, twi * 0.65f);
    mix3(amb, night_amb, day_amb, day);
    /* the sun (orange low, white high) fades out across the horizon as the moon, opposite, fades in; the light comes
     * from the stronger one, so the direction swaps where both are faint */
    float sun_s = smooth(-0.05f, 0.18f, e), moon_s = smooth(-0.05f, 0.18f, -e) * 0.45f;
    if (sun_s >= moon_s) {
        mix3(sc, low, noon, smooth(0.02f, 0.45f, e));
        for (int k = 0; k < 3; k++) { sc[k] *= sun_s; s->sun[k] = sun[k]; }
    } else
        for (int k = 0; k < 3; k++) { sc[k] = moon[k] * moon_s; s->sun[k] = -sun[k]; }
    /* weather: grey toward luminance, dim the sun, lift the ambient a little */
    float o = w->overcast, tint[3];
    unpack(w->tint, tint);
    float *cols[4] = {zen, hor, amb, sc};
    for (int c = 0; c < 4; c++) {
        float *v = cols[c], l = 0.3f * v[0] + 0.59f * v[1] + 0.11f * v[2];
        float target = c == 3 ? l * 0.25f : c == 2 ? l * 1.15f + 6 : l * 0.85f;
        float grey = c == 3 ? 0.9f : c == 2 ? 0.75f : 0.95f;   /* sun, ambient, sky colours */
        for (int k = 0; k < 3; k++) v[k] = lerpf(v[k], target, o * grey) * tint[k] / 255.0f;
    }
    s->zenith = pack(zen);
    s->horizon = pack(hor);
    s->fog_color = s->horizon;
    s->ambient = pack(amb);
    s->sun_color = pack(sc);
    s->fog_near = w->fog_near;
    s->fog_far = w->fog_far * lerpf(0.75f, 1.0f, day);   /* the night closes in a little */

    /* relight when the light moved or changed enough */
    float dot = s->sun[0] * s->lit_sun[0] + s->sun[1] * s->lit_sun[1] + s->sun[2] * s->lit_sun[2];
    int changed = dot < 0.99966f;   /* 1.5 degrees */
    for (int k = 0; k < 24 && !changed; k += 8)
        changed = abs((int)(s->sun_color >> k & 255) - (int)(s->lit_color >> k & 255)) > 4 ||
                  abs((int)(s->ambient >> k & 255) - (int)(s->lit_ambient >> k & 255)) > 4;
    if (changed || !s->serial) {
        s->serial++;
        if (!s->serial) s->serial = 1;
        memcpy(s->lit_sun, s->sun, sizeof s->sun);
        s->lit_color = s->sun_color;
        s->lit_ambient = s->ambient;
    }
}

void vcpe_sky_init(VcpeSky *s, float hour, float day_seconds)
{
    memset(s, 0, sizeof *s);
    s->hour = fmodf(hour, 24);
    if (s->hour < 0) s->hour += 24;
    s->day_seconds = day_seconds;
    s->tilt = 45;
    vcpe_weather_preset(&s->from, VCPE_WEATHER_CLEAR);
    s->to = s->now = s->from;
    s->blend = 1;
    compute(s);
}

void vcpe_sky_weather(VcpeSky *s, const VcpeWeather *w, float seconds)
{
    s->from = s->now;      /* from what shows now: no jump mid-transition */
    s->to = *w;
    s->blend = seconds > 0 ? 0 : 1;
    s->rate = seconds > 0 ? 1 / seconds : 0;
    compute(s);
}

void vcpe_sky_update(VcpeSky *s, float dt)
{
    if (!(dt > 0))
        return;
    s->time += dt;
    if (s->time >= 4096) s->time -= 4096;   /* bounded, so float precision holds; precipitation jumps once */
    if (s->day_seconds > 0) {
        s->hour += dt * 24 / s->day_seconds;
        while (s->hour >= 24) s->hour -= 24;
    }
    if (s->blend < 1)
        s->blend = s->rate > 0 ? fminf(1, s->blend + dt * s->rate) : 1;
    compute(s);
}

void vcpe_sky_set_hour(VcpeSky *s, float hour)
{
    s->hour = fmodf(hour, 24);
    if (s->hour < 0) s->hour += 24;
    compute(s);
}

void vcpe_sky_light(const VcpeSky *s, VcpeTerrainLight *out)
{
    memcpy(out->sun, s->lit_sun, sizeof out->sun);
    out->sun_color = s->lit_color;
    out->ambient = s->lit_ambient;
    out->serial = s->serial;
}

void vcpe_sky_backdrop(const VcpeSky *s, VcpeBackdrop *b)
{
    b->haze = s->horizon;
    b->zenith = s->zenith;
    /* distant rock and snow lit like the world: ambient + sun at its elevation */
    float a[3], c[3], rock[3] = {111, 116, 119}, snow[3] = {230, 234, 235};
    unpack(s->ambient, a);
    unpack(s->sun_color, c);
    float e = s->sun[1] > 0 ? s->sun[1] : 0;
    for (int k = 0; k < 3; k++) {
        float l = (a[k] + c[k] * (0.4f + 0.6f * e)) / 255.0f;
        rock[k] *= l > 1.2f ? 1.2f : l;
        snow[k] *= l > 1.1f ? 1.1f : l;
    }
    b->rock = pack(rock);
    b->snow = pack(snow);
}

/* ------------------------------------------------------------------ precipitation */
static uint32_t hash(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
static float frand(uint32_t i, uint32_t salt) { return (float)(hash(i * 0x9e3779b9u + salt) >> 8) * (1.0f / 16777216.0f); }
static float wrap(float v, float lo, float size)
{
    float r = fmodf(v - lo, size);
    return (r < 0 ? r + size : r) + lo;
}

#define BOX_W 24.0f
#define BOX_H 14.0f
#define BOX_Y0 -4.0f

int vcpe_sky_precip_points(const VcpeSky *s, const float cam[3], float (*out)[3], int n)
{
    const VcpeWeather *w = &s->now;
    if (w->precip_kind == VCPE_PRECIP_NONE || w->precip <= 0 || n <= 0)
        return 0;
    int snow = w->precip_kind == VCPE_PRECIP_SNOW;
    float t = s->time;
    for (int i = 0; i < n; i++) {
        uint32_t u = (uint32_t)i;
        float speed = snow ? 0.9f + 0.6f * frand(u, 1) : 8.0f + 3.0f * frand(u, 1);
        float x = frand(u, 3) * BOX_W + fmodf(w->wind[0] * t, BOX_W);
        float z = frand(u, 5) * BOX_W + fmodf(w->wind[1] * t, BOX_W);
        float y = frand(u, 4) * BOX_H - fmodf(t, BOX_H / speed) * speed;
        if (snow) {
            /* flakes sway on their own small circles */
            float f = 0.5f + 0.7f * frand(u, 6), ph = 6.2831853f * frand(u, 7), a = 6.2831853f * f * fmodf(t, 1 / f) + ph;
            x += 0.3f * sinf(a);
            z += 0.3f * cosf(a * 0.8f);
        }
        out[i][0] = wrap(x, cam[0] - BOX_W * 0.5f, BOX_W);
        out[i][1] = wrap(y, cam[1] + BOX_Y0, BOX_H);
        out[i][2] = wrap(z, cam[2] - BOX_W * 0.5f, BOX_W);
    }
    return n;
}
