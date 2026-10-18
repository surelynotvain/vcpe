/* Bounded rectangular cells for shallow first-person scenes. X/Z floor plan,
 * Y up. Content owns arrays. No allocator, PSP calls, meshes or game logic. */
#ifndef VCPE_SECTOR_H
#define VCPE_SECTOR_H
#include <stdint.h>
typedef struct { float x0,z0,x1,z1,floor,ceiling; } VcpeSectorCell;
typedef struct { int a,b,open; float x0,z0,x1,z1; } VcpeSectorPortal;
typedef struct {
    const VcpeSectorCell *cells; int count;
    const VcpeSectorPortal *portals; int portal_count;
} VcpeSectors;
typedef struct {
    float x,y,z,radius,height,step_up,step_down;
    int cell;
} VcpeSectorBody;
int vcpe_sector_validate(const VcpeSectors *s); /* max 64 cells / 256 portals */
/* Conservative connectivity mask, including doors only when open. Geometry
 * projection still performs frustum clipping. Disconnected cells need no draws. */
uint64_t vcpe_sector_connected(const VcpeSectors *s,int cell);
/* Collision with radius, portal width, headroom and step limits. Substeps prevent
 * tunnelling; X/Z separation slides along walls. Returns -1 invalid/overlarge
 * request (body unchanged), 0 blocked, 1 moved. Call at a fixed simulation rate.
 * Door changes need no mesh rebuild; content draws its own closed-door cards. */
int vcpe_sector_move(const VcpeSectors *s,VcpeSectorBody *b,float dx,float dz);
#endif
