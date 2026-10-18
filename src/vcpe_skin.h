/* Generic CPU skinning for small PSP rigs. Clip matrices are converted offline
 * from source animation, expressed as delta-from-bind affine transforms. */
#ifndef VCPE_SKIN_H
#define VCPE_SKIN_H
#include <stdint.h>
#include "vcpe_gfx.h"
#define VCPE_SKIN_MAX_BONES 192
typedef struct {float xyz[3];uint16_t bone[4];float weight[4];} VcpeSkinVertex;
/* Keys: frames * bones * 12 floats (3x4 affine, row-major). */
typedef struct {const void *keys;int frames,bones;float duration;int half;} VcpeSkinClip;
int vcpe_skin_pose(const VcpeSkinClip *clip,float seconds,int loop,float out[VCPE_SKIN_MAX_BONES][12]);
/* PSP VFPU acceleration is opt-in; caller thread needs THREAD_ATTR_VFPU.
 * Unaligned pose arrays and non-PSP hosts retain the scalar implementation. */
void vcpe_skin_accelerate(int on);
int vcpe_skin_apply(VcpeVtx *vertices,const VcpeSkinVertex *bind,int count,const float (*pose)[12],int bones);
int vcpe_skin_aim(float (*pose)[12],int count,int head,const unsigned char *mask,const float bind[12],const float target[3],float max_radians);
#endif
