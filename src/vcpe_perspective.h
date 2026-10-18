/* Shallow perspective scenes made from flat textured cards. No depth buffer, 3D GU
 * matrices, dynamic allocation or PSP dependency in this module. Coordinates: Y up,
 * yaw 0 looks down -Z; UVs normalized to a texture page. Link vcpe_perspective_draw
 * for the PSP 2D-triangle backend. Intersecting cards require authored splits. */
#ifndef VCPE_PERSPECTIVE_H
#define VCPE_PERSPECTIVE_H
#include "vcpe_gfx.h"
typedef struct {
    float pos[3], yaw, pitch, fov, near_z, far_z;
    int width, height;
} VcpePerspectiveCamera;
typedef struct {
    VcpeVtx v[3];
    float depth;
    int page;
    unsigned order;
} VcpePerspectiveTriangle;
typedef struct {
    VcpePerspectiveCamera camera;
    VcpePerspectiveTriangle *triangles;
    unsigned capacity, count, cards, rejected, dropped, submitted;
    float right[3], up[3], forward[3], focal;
    int valid, sorted;
} VcpePerspective;
/* Storage belongs to the caller, normally reserved once at loading time. Invalid
 * camera/storage returns -1. begin resets every counter, including overflow. */
int vcpe_perspective_begin(VcpePerspective *p, const VcpePerspectiveCamera *camera,
                          VcpePerspectiveTriangle *storage, unsigned capacity);
/* Four corners in perimeter order. Clips against all six view planes before
 * projection; returns emitted triangles. Subdivide long slanted cards in content
 * to limit affine texture distortion. A card that exceeds capacity is discarded
 * atomically and counted in dropped. No hidden allocation or silent overwrite. */
int vcpe_perspective_card(VcpePerspective *p, int page, const VcpeVtx corners[4]);
/* Upright camera-facing cutout, feet at position; optional subrectangle in a
 * multi-direction atlas. heading: 0 faces -Z. views 1..32. Returns view index. */
int vcpe_perspective_view(float camera_x, float camera_z, float actor_x,
                         float actor_z, float heading, int views);
int vcpe_perspective_sprite(VcpePerspective *p, int page, const float feet[3],
                           float width, float height, const float uv[4], uint32_t color);
void vcpe_perspective_sort(VcpePerspective *p); /* stable tie order; far to near */
/* PSP adapter: inside gfx_begin/end, before HUD. No depth buffer is allocated or
 * enabled. Consumes projected queue, does not retain its CPU pointers. Returns
 * drawn triangles; list exhaustion increments dropped. No 3D modules required. */
int vcpe_perspective_draw(VcpePerspective *p);
enum { VCPE_PERSPECTIVE_OPAQUE = 1 }; /* alpha-tested cards, no translucent surfaces */
int vcpe_perspective_draw_ex(VcpePerspective *p, unsigned flags);
#endif
