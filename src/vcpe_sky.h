/* VCPE sky: time of day and weather for open worlds. One update per tick gives the sun (or moon) direction and
 * colours, ambient light, sky zenith/horizon, fog colour and distance, all blended between weathers over a
 * transition; the game passes them on (vcpe_3d_fog, vcpe_backdrop, vcpe_terrain lighting). vcpe_sky_draw.c adds
 * rain and snow around the camera.
 *
 *   VcpeSky sky;  vcpe_sky_init(&sky, 8.0f, 72 * 60);            // 8:00, a game day every 72 real minutes
 *   VcpeWeather snow;  vcpe_weather_preset(&snow, VCPE_WEATHER_SNOW);
 *   vcpe_sky_weather(&sky, &snow, 30);                            // turns to snow over 30 s
 *   each tick:  vcpe_sky_update(&sky, dt);
 *   each frame: vcpe_3d_fog(sky.fog_near, sky.fog_far, sky.fog_color);
 *               if (sky.serial != my_serial) { vcpe_sky_light(&sky, &light); ... vcpe_terrain_relight_some ... }
 *               vcpe_sky_backdrop(&sky, &bd); vcpe_backdrop_recolor(&bd) (every second or so); vcpe_backdrop_draw(...)
 *               inside the world's 3D pass: vcpe_sky_draw_precip(&sky, cam, rot, -1, 400);
 *
 * Deterministic: everything follows from the hour, the weathers and the accumulated update time, so autotest
 * recordings stay frame-exact and a paused game (dt 0) freezes rain in the air. Plain C except vcpe_sky_draw.c. */
#ifndef VCPE_SKY_H
#define VCPE_SKY_H
#include <stdint.h>
#include "vcpe_backdrop.h"
#include "vcpe_terrain.h"

enum { VCPE_PRECIP_NONE, VCPE_PRECIP_RAIN, VCPE_PRECIP_SNOW };
enum { VCPE_WEATHER_CLEAR, VCPE_WEATHER_CLOUDY, VCPE_WEATHER_OVERCAST, VCPE_WEATHER_RAIN, VCPE_WEATHER_STORM,
       VCPE_WEATHER_SNOW, VCPE_WEATHER_BLIZZARD, VCPE_WEATHER_FOG, VCPE_WEATHER_COUNT };

typedef struct {
    float overcast;                /* 0 clear .. 1 a grey sky without sun */
    float fog_near, fog_far;       /* metres */
    float precip;                  /* 0..1 of the drops/flakes vcpe_sky_draw_precip may draw */
    int precip_kind;               /* VCPE_PRECIP_*; a transition switches kind halfway */
    float wind[2];                 /* m/s along x and z (drift of rain and snow) */
    uint32_t tint;                 /* ABGR multiplier on sky, fog and light (0xffffffff: none) */
} VcpeWeather;
void vcpe_weather_preset(VcpeWeather *w, int preset);

typedef struct {
    float hour;                    /* 0..24 */
    float day_seconds;             /* real seconds per game day; 0 stops the clock */
    float tilt;                    /* the sun's noon distance from the zenith, degrees (45: northern) */
    float yaw;                     /* sunrise direction around Y, degrees (0: +X) */
    float time;                    /* seconds of update since init (drives rain and snow) */
    VcpeWeather from, to, now;
    float blend, rate;             /* weather transition 0..1, per second */
    /* outputs */
    float sun[3];                  /* toward the light: the sun by day, the moon by night */
    float sun_elev;                /* sine of the sun's elevation (negative at night) */
    float light_dir_sun[3];        /* toward the sun itself, always (for a sun disc) */
    uint32_t zenith, horizon, fog_color, sun_color, ambient;
    float fog_near, fog_far;
    uint32_t serial;               /* changes when the light moved enough to relight terrain (> 1.5 degrees) */
    float lit_sun[3];
    uint32_t lit_color, lit_ambient;
} VcpeSky;

void vcpe_sky_init(VcpeSky *s, float hour, float day_seconds);
/* change to w over seconds (0: at once) from whatever is showing now */
void vcpe_sky_weather(VcpeSky *s, const VcpeWeather *w, float seconds);
void vcpe_sky_update(VcpeSky *s, float dt);
void vcpe_sky_set_hour(VcpeSky *s, float hour);    /* sleeping, waiting: jumps, relights */
void vcpe_sky_light(const VcpeSky *s, VcpeTerrainLight *out);
/* haze and zenith from the sky; rock and snow hazed toward the light (the skyline stays) */
void vcpe_sky_backdrop(const VcpeSky *s, VcpeBackdrop *b);

/* GE side (vcpe_sky_draw.c, needs vcpe_3d), inside the world's 3D pass after opaque geometry: rain streaks or snow
 * flakes in a 24 x 14 x 24 m box that follows the camera. cam: the camera position given to vcpe_3d_begin; rot: its
 * rotation (degrees); page: flake/drop texture (-1: untextured); max: drawn at full precipitation (<= 1024).
 * Returns particles drawn. Display-list memory: 72 bytes a particle. */
int vcpe_sky_draw_precip(const VcpeSky *s, const float cam[3], const float rot[3], int page, int max);
/* the particle positions vcpe_sky_draw_precip uses (for tests and splash effects): n of them into out */
int vcpe_sky_precip_points(const VcpeSky *s, const float cam[3], float (*out)[3], int n);

#endif
