/* VCPE vehicle: a towed wagon/carriage on a VcpePath. The front axle (model origin) rides the path,
 * the rear axle trails it over the wheelbase so the body cuts corners like a real towed vehicle,
 * pitch follows the road grade and roll/heave come from deterministic rut bumps scaled by speed.
 * Update it with simulation ticks only: a paused game then keeps the pose. */
#ifndef VCPE_VEHICLE_H
#define VCPE_VEHICLE_H
#include "vcpe_path.h"
typedef struct {
    float wheelbase;          /* metres between axles */
    float bump_pitch, bump_roll, bump_heave; /* amplitudes (degrees, degrees, metres); 0 = smooth road */
    float speed_ref;          /* metres per tick that count as full speed for bumps */
    /* state */
    float pos[3], rear[2], yaw, pitch, roll, heave, distance, speed;
    int valid;
    float matrix[16];         /* row-major model matrix, Ry(yaw) Rx(pitch) Rz(roll), for vcpe_3d_model */
} VcpeVehicle;
void vcpe_vehicle_init(VcpeVehicle *v, float wheelbase);
/* Move to distance d (front axle) after `ticks` simulation ticks; reset = snap without trailing. */
void vcpe_vehicle_update(VcpeVehicle *v, const VcpePath *path, float d, int ticks, int reset);
/* Row-major Ry(yaw) * Rx(pitch) * Rz(roll) with translation, the vcpe_3d_model convention. */
void vcpe_matrix_ypr(float out[16], const float pos[3], float yaw, float pitch, float roll);
void vcpe_matrix_point(const float m[16], const float p[3], float out[3]);
#endif
