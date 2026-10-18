/* VCPE backdrop: cheap long-range view faking for 3D scenes. A camera-centred sky dome and up to two
 * ridge-silhouette rings (procedural, snow-capped, hazed toward the fog colour) are drawn in their own
 * depth pass before the world. Real geometry then fades into fog in front of "distant" mountains that
 * have no parallax, like a skybox. Call vcpe_backdrop_draw(rot) right before vcpe_3d_begin for the world. */
#ifndef VCPE_BACKDROP_H
#define VCPE_BACKDROP_H
#include <stdint.h>
typedef struct {
    uint32_t haze, zenith;      /* horizon (match the world fog colour) and sky top, ABGR */
    uint32_t rock, snow;        /* ridge base and cap colours before hazing */
    int layers;                 /* 0..2 ridge rings */
    float height;               /* overall ridge height scale, 1 = default */
    unsigned seed;              /* changes the skyline */
} VcpeBackdrop;
void vcpe_backdrop_defaults(VcpeBackdrop *b);   /* overcast northern sky */
void vcpe_backdrop_build(const VcpeBackdrop *b);
void vcpe_backdrop_draw(const float rot_deg[3], float fov_deg);   /* fog off, own depth clear */
#endif
