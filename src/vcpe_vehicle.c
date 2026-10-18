#include "vcpe_vehicle.h"
#include "vcpe_math.h"
#include <string.h>

void vcpe_matrix_ypr(float out[16], const float pos[3], float yaw, float pitch, float roll)
{
    float cy = vcpe_cos_deg(yaw), sy = vcpe_sin_deg(yaw), cp = vcpe_cos_deg(pitch), sp = vcpe_sin_deg(pitch);
    float cr = vcpe_cos_deg(roll), sr = vcpe_sin_deg(roll);
    out[0] = cy * cr + sy * sp * sr; out[1] = -cy * sr + sy * sp * cr; out[2] = sy * cp; out[3] = pos[0];
    out[4] = cp * sr; out[5] = cp * cr; out[6] = -sp; out[7] = pos[1];
    out[8] = -sy * cr + cy * sp * sr; out[9] = sy * sr + cy * sp * cr; out[10] = cy * cp; out[11] = pos[2];
    out[12] = out[13] = out[14] = 0; out[15] = 1;
}

void vcpe_matrix_point(const float m[16], const float p[3], float out[3])
{
    for (int k = 0; k < 3; k++)
        out[k] = m[k * 4] * p[0] + m[k * 4 + 1] * p[1] + m[k * 4 + 2] * p[2] + m[k * 4 + 3];
}

void vcpe_vehicle_init(VcpeVehicle *v, float wheelbase)
{
    memset(v, 0, sizeof *v);
    v->wheelbase = wheelbase > .1f ? wheelbase : .1f;
    v->bump_pitch = .9f; v->bump_roll = 1.6f; v->bump_heave = .035f; v->speed_ref = 1 / 12.0f;
    const float zero[3] = {0, 0, 0};
    vcpe_matrix_ypr(v->matrix, zero, 0, 0, 0);
}

/* Uneven ruts: incommensurate sines plus a sharp stone every few metres. */
static float bump(float d, float a, float b, float c)
{
    float s = vcpe_sin_deg(d * a) * .55f + vcpe_sin_deg(d * b + 40) * .3f;
    float stone = vcpe_sin_deg(d * c);
    return s + (stone > .93f ? (stone - .93f) * 14 : 0);
}

void vcpe_vehicle_update(VcpeVehicle *v, const VcpePath *path, float d, int ticks, int reset)
{
    float front[3], back[3], yaw;
    vcpe_path_sample(path, d, 1, front, &yaw);
    if (reset || !v->valid || vcpe_absf(d - v->distance) > v->wheelbase) {
        vcpe_path_point(path, d - v->wheelbase, back);
        v->rear[0] = back[0]; v->rear[1] = back[2];
        v->valid = 1;
        v->distance = d;
        v->speed = 0;
    }
    float dx = front[0] - v->rear[0], dz = front[2] - v->rear[1], len = vcpe_sqrt(dx * dx + dz * dz);
    if (len > .001f) {
        dx /= len; dz /= len;
        v->rear[0] = front[0] - dx * v->wheelbase;
        v->rear[1] = front[2] - dz * v->wheelbase;
        v->yaw = vcpe_atan2(-dx, -dz) * 180 / VCPE_PI;
    }
    float moving = vcpe_clampf(vcpe_absf(d - v->distance) / (ticks > 0 ? ticks : 1) / v->speed_ref, 0, 1);
    v->speed += (moving - v->speed) * (reset ? 1 : .08f);
    v->distance = d;
    vcpe_path_point(path, d - v->wheelbase, back);
    v->pos[0] = front[0]; v->pos[1] = front[1]; v->pos[2] = front[2];
    v->pitch = vcpe_atan2(front[1] - back[1], v->wheelbase) * 180 / VCPE_PI + bump(d, 97, 233, 61) * v->bump_pitch * v->speed;
    v->roll = bump(d, 71, 181, 83) * v->bump_roll * v->speed;
    v->heave = vcpe_absf(bump(d, 149, 311, 61)) * v->bump_heave * v->speed;
    float p[3] = {v->pos[0], v->pos[1] + v->heave, v->pos[2]};
    vcpe_matrix_ypr(v->matrix, p, v->yaw, v->pitch, v->roll);
}
