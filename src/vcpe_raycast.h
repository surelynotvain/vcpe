/* Small 2.5D wall-strip projector. No PSP dependency, allocations, triangle
 * queue, matrices or framebuffer depth buffer. X/Z plane, Y up, yaw 0 = -Z. */
#ifndef VCPE_RAYCAST_H
#define VCPE_RAYCAST_H
#include <stdint.h>
typedef struct {float x0,z0,x1,z1,bottom,top;int page;uint32_t color;} VcpeRayWall;
typedef struct {float x,z,eye,yaw,pitch,fov,near_z,far_z;int width,height;} VcpeRayView;
typedef struct {float depth,tex_u,top,bottom;int wall;} VcpeRayColumn;
/* Caller supplies ceil(width/strip_width) columns (1..480). Returns count, -1
 * invalid input. depth is perpendicular view distance, suitable for sprite
 * occlusion. It is one value per strip, not a per-pixel depth buffer. */
int vcpe_raycast(const VcpeRayView *view,const VcpeRayWall *walls,int wall_count,
                 int strip_width,VcpeRayColumn *columns,int capacity);
#endif
