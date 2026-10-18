/* vcpe_sky host test: sun path, day/night colours, moonlight, weather transitions, relight serials, precipitation. */
#include "vcpe_sky.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static int R(uint32_t c) { return (int)(c & 255); }
static int G(uint32_t c) { return (int)(c >> 8 & 255); }
static int B(uint32_t c) { return (int)(c >> 16 & 255); }
static int lum(uint32_t c) { return (R(c) * 3 + G(c) * 6 + B(c)) / 10; }

int main(void)
{
    VcpeSky s;
    vcpe_sky_init(&s, 6, 0);
    CHECK(s.light_dir_sun[0] > 0.99f && fabsf(s.light_dir_sun[1]) < 1e-3f, "sunrise in the east (+X)");
    vcpe_sky_set_hour(&s, 12);
    CHECK(fabsf(s.sun_elev - cosf(45 * 3.14159265f / 180)) < 1e-3f && s.light_dir_sun[2] > 0.7f, "noon: 45 degrees from the zenith, toward +Z");
    CHECK(B(s.zenith) > R(s.zenith) + 60 && lum(s.horizon) > 150, "blue noon sky 0x%08x, bright horizon", s.zenith);
    CHECK(fabsf(s.sun[0] - s.light_dir_sun[0]) < 1e-6f && lum(s.sun_color) > 200, "lit by the sun");
    uint32_t noon_sun = s.sun_color, noon_amb = s.ambient;
    vcpe_sky_set_hour(&s, 0);
    CHECK(s.sun_elev < -0.6f && lum(s.zenith) < 30 && lum(s.horizon) < 50, "dark night sky");
    CHECK(s.sun[1] > 0.6f && s.sun[1] == -s.light_dir_sun[1] && B(s.sun_color) > R(s.sun_color) && lum(s.sun_color) < 60, "moonlight from opposite the sun, dim and blue");
    CHECK(lum(s.ambient) < lum(noon_amb), "darker ambient at night");
    vcpe_sky_set_hour(&s, 18.1f);
    CHECK(R(s.horizon) > B(s.horizon) + 40, "orange sunset horizon 0x%08x", s.horizon);
    /* smooth through the day: no colour jumps between 1-minute steps */
    int worst = 0;
    uint32_t prev = 0;
    for (float h = 0; h < 24; h += 1 / 60.0f) {
        vcpe_sky_set_hour(&s, h);
        if (h > 0) {
            int d = abs(R(s.horizon) - R(prev)) + abs(G(s.horizon) - G(prev)) + abs(B(s.horizon) - B(prev));
            if (d > worst) worst = d;
        }
        prev = s.horizon;
    }
    CHECK(worst <= 6, "horizon changes at most %d per game minute", worst);

    /* the clock: 240 s days */
    vcpe_sky_init(&s, 10, 240);
    for (int i = 0; i < 600; i++) vcpe_sky_update(&s, 1 / 60.0f);
    CHECK(fabsf(s.hour - 11) < 1e-3f, "an hour in 10 s (%g)", s.hour);
    float hour = s.hour;
    vcpe_sky_update(&s, 0);
    CHECK(s.hour == hour, "dt 0 stops the clock");

    /* overcast greys the sky and dims the sun */
    vcpe_sky_init(&s, 12, 0);
    VcpeWeather w;
    vcpe_weather_preset(&w, VCPE_WEATHER_OVERCAST);
    vcpe_sky_weather(&s, &w, 0);
    CHECK(lum(s.sun_color) < lum(noon_sun) / 2 && abs(R(s.zenith) - B(s.zenith)) < 40, "overcast: dim sun, grey sky");

    /* transitions: clear -> snow over 10 s */
    vcpe_sky_init(&s, 12, 0);
    VcpeWeather snow, rain, clear;
    vcpe_weather_preset(&snow, VCPE_WEATHER_SNOW);
    vcpe_weather_preset(&rain, VCPE_WEATHER_RAIN);
    vcpe_weather_preset(&clear, VCPE_WEATHER_CLEAR);
    vcpe_sky_weather(&s, &snow, 10);
    CHECK(s.now.precip == 0 && s.fog_far > 400, "starts clear");
    for (int i = 0; i < 300; i++) vcpe_sky_update(&s, 1 / 60.0f);
    CHECK(s.now.precip_kind == VCPE_PRECIP_SNOW && s.now.precip > 0.2f && s.now.precip < 0.4f && s.fog_far < 400 && s.fog_far > 160, "halfway: some snow, closer fog (%g, %g m)", s.now.precip, s.fog_far);
    /* a new request mid-transition starts from what shows: no jump */
    uint32_t z = s.zenith;
    float far = s.fog_far;
    vcpe_sky_weather(&s, &rain, 10);
    CHECK(s.zenith == z && fabsf(s.fog_far - far) < 1e-3f, "redirected without a jump");
    /* snow -> rain switches kind halfway, fading through zero */
    vcpe_sky_init(&s, 12, 0);
    vcpe_sky_weather(&s, &snow, 0);
    vcpe_sky_weather(&s, &rain, 10);
    float low = 1;
    int kinds = 0, last = s.now.precip_kind;
    for (int i = 0; i < 600; i++) {
        vcpe_sky_update(&s, 1 / 60.0f);
        if (s.now.precip < low) low = s.now.precip;
        if (s.now.precip_kind != last) { kinds++; last = s.now.precip_kind; }
    }
    CHECK(kinds == 1 && last == VCPE_PRECIP_RAIN && low < 0.01f && fabsf(s.now.precip - rain.precip) < 1e-4f, "snow fades out, rain fades in (%d swaps, low %g)", kinds, low);
    vcpe_sky_weather(&s, &clear, 2);
    for (int i = 0; i < 180; i++) vcpe_sky_update(&s, 1 / 60.0f);
    CHECK(s.now.precip == 0 && s.now.precip_kind == VCPE_PRECIP_NONE, "back to clear");

    /* relight serials: a 72-minute day moves the sun 1.5 degrees in about 18 s */
    vcpe_sky_init(&s, 9, 72 * 60);
    uint32_t serial = s.serial;
    int bumps = 0;
    for (int i = 0; i < 360 * 60; i++) {
        vcpe_sky_update(&s, 1 / 60.0f);
        if (s.serial != serial) { bumps++; serial = s.serial; }
    }
    CHECK(bumps >= 12 && bumps <= 30, "%d relights in 6 real minutes", bumps);
    VcpeTerrainLight l;
    vcpe_sky_light(&s, &l);
    CHECK(l.serial == s.serial && l.sun_color == s.lit_color && fabsf(l.sun[0] - s.lit_sun[0]) < 1e-6f, "terrain light");
    VcpeBackdrop bd = {0, 0, 0, 0, 2, 1, 0};   /* vcpe_backdrop_defaults lives with the GE code */
    vcpe_sky_backdrop(&s, &bd);
    CHECK(bd.haze == s.horizon && bd.zenith == s.zenith && bd.layers == 2 && lum(bd.snow) > lum(bd.rock), "backdrop colours, skyline kept");

    /* precipitation: in the box around the camera, frozen by a pause, falling otherwise, deterministic */
    vcpe_sky_init(&s, 12, 0);
    vcpe_sky_weather(&s, &snow, 0);
    vcpe_sky_update(&s, 3.7f);
    static float a[400][3], b[400][3], c[400][3];
    float cam[3] = {1000.5f, 52, -300.25f};
    CHECK(vcpe_sky_precip_points(&s, cam, a, 400) == 400, "400 flakes");
    int inside = 1;
    for (int i = 0; i < 400; i++)
        inside &= fabsf(a[i][0] - cam[0]) <= 12 && fabsf(a[i][2] - cam[2]) <= 12 && a[i][1] >= cam[1] - 4 && a[i][1] <= cam[1] + 10;
    CHECK(inside, "all inside the camera box");
    vcpe_sky_update(&s, 0);
    vcpe_sky_precip_points(&s, cam, b, 400);
    CHECK(!memcmp(a, b, sizeof a), "a pause freezes them");
    vcpe_sky_update(&s, 1 / 60.0f);
    vcpe_sky_precip_points(&s, cam, c, 400);
    int down = 0;
    for (int i = 0; i < 400; i++) down += c[i][1] < a[i][1] && a[i][1] - c[i][1] < 0.1f;
    CHECK(down > 380, "%d of 400 fell a little", down);
    VcpeSky s2;
    vcpe_sky_init(&s2, 12, 0);
    vcpe_sky_weather(&s2, &snow, 0);
    for (int i = 0; i < 223; i++) vcpe_sky_update(&s2, 1 / 60.0f);
    vcpe_sky_precip_points(&s2, cam, b, 400);
    vcpe_sky_init(&s, 12, 0);
    vcpe_sky_weather(&s, &snow, 0);
    for (int i = 0; i < 223; i++) vcpe_sky_update(&s, 1 / 60.0f);
    vcpe_sky_precip_points(&s, cam, c, 400);
    CHECK(!memcmp(b, c, sizeof b), "the same run draws the same");
    float moved[3] = {cam[0] + 5, cam[1], cam[2]};
    vcpe_sky_precip_points(&s, moved, b, 400);
    int same = 0;
    for (int i = 0; i < 400; i++) same += fabsf(b[i][0] - c[i][0]) < 1e-3f && fabsf(b[i][2] - c[i][2]) < 1e-3f;
    CHECK(same > 250 && same < 400, "flakes stay put in the world as the camera moves (%d kept, the rest wrapped)", same);
    vcpe_sky_weather(&s, &clear, 0);
    CHECK(vcpe_sky_precip_points(&s, cam, a, 400) == 0, "none when clear");

    printf("PASS sky: sun path, day/night/twilight colours, moonlight, clock, overcast, transitions, relight serials, precipitation\n");
    printf("%d checks, %d failures\n", checks, fails);
    return fails != 0;
}
