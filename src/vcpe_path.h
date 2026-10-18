/* VCPE path: arc-length sampling of a polyline as a Catmull-Rom curve, with a stable heading.
 * Good for rails, carts, camera tracks and NPC walks recovered from source marker chains.
 * Points are borrowed (not copied); keep them alive while the path is used. */
#ifndef VCPE_PATH_H
#define VCPE_PATH_H
#define VCPE_PATH_MAX 1024
typedef struct {
    const float (*points)[3];
    int count;
    float cumulative[VCPE_PATH_MAX]; /* XZ arc length at each point */
} VcpePath;
int vcpe_path_init(VcpePath *p, const float (*points)[3], int count);  /* 0 ok, -1 bad input or zero length */
float vcpe_path_length(const VcpePath *p);
/* Position at distance d along the path; beyond the end it extends along the last segment. */
void vcpe_path_point(const VcpePath *p, float d, float out[3]);
/* Position plus heading in degrees, VCPE camera/model convention (forward is -Z, yaw = atan2(-dx,-dz)).
 * The heading is taken over +-half_base metres so small kinks in the source points don't jitter it. */
void vcpe_path_sample(const VcpePath *p, float d, float half_base, float out[3], float *yaw_deg);
#endif
