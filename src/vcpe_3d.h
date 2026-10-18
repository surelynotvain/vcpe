/* VCPE 3D: perspective rendering on the GU. A 16-bit depth buffer in VRAM (only once a game asks for 3D),
 * a camera given like a Godot Camera3D (position, YXZ euler angles in degrees, vertical FOV), and indexed
 * meshes in world space: VcpeVtx vertices (float UV, ABGR colour, float position) with 16-bit indices.
 * Draw the 3D scene between vcpe_3d_begin() and vcpe_3d_end(), then 2D (HUD) as before. */
#ifndef VCPE_3D_H
#define VCPE_3D_H
#include <stdint.h>
#include "vcpe_cull.h"

void vcpe_3d_begin(const float pos[3], const float rot_deg[3], float fov_deg, float znear, float zfar);
void vcpe_3d_model(const float *m16);   /* row-major 4x4 model matrix for the next draws; 0 = identity */
void vcpe_3d_draw(int page, const void *verts, const uint16_t *idx, int nidx);   /* page < 0: untextured */
void vcpe_3d_fog(float start, float end, uint32_t color);
void vcpe_3d_filter(int linear); /* opt-in smooth magnification + trilinear mip sampling */
void vcpe_3d_material(int blend,int alpha_threshold); /* per-texel alpha; blended surfaces follow opaque ones */
void vcpe_3d_add(float k);     /* next draws: added on top, k x their colour (a light layer over the same surfaces); vcpe_3d_alpha(1) ends it */
void vcpe_3d_alpha(float a);   /* next draws: whole-object opacity (fixed blend factors); 1 = opaque */                         /* end <= start: off */
void vcpe_3d_end(void);
/* the camera of the last vcpe_3d_begin: can it see a world-space sphere? (skip drawing what it cannot) */
int vcpe_3d_visible(const float center[3], float radius);
int vcpe_3d_project(const float p[3], float *sx, float *sy);   /* world point -> screen pixels of the last camera; 0: behind it */

/* Link vcpe_cull for automatic CPU rejection before texture binding/GE submission. Scene nodes prepare their
 * batches themselves. Raw draw users prepare immutable geometry during loading; all LOD lists need preparation.
 * Cache misses/allocation failures draw normally. Invalidate mutable vertices (all shared LODs) after edits.
 * reset before freeing/replacing assets; a scene load resets the shared cache, so prepare raw batches afterward. */
int vcpe_3d_prepare(const void *verts, uint32_t nverts, const uint16_t *idx, uint32_t nidx);
void vcpe_3d_geometry_changed(const void *verts);
void vcpe_3d_cull_reset(void);
void vcpe_3d_autocull(int on);                 /* enabled by default when vcpe_cull is linked; 0: A/B bypass */
int vcpe_3d_autocull_enabled(void);
int vcpe_3d_visible_local(const float center[3], float radius, const float *model16);
int vcpe_3d_visible_box(const VcpeCullBox *box, const float *model16);
int vcpe_3d_visible_points(const void *verts, uint32_t count, const float *model16); /* transient VcpeVtx list */
uint32_t vcpe_3d_cull_bytes(void);

/* LOD meshes (vcpe_lod.h): the last camera as a picker view (thresh_px: error allowed on screen), and drawing one
 * level of a mesh with the current model matrix */
#include "vcpe_lod.h"
void vcpe_3d_lod_view(VcpeLodView *v, float thresh_px);
void vcpe_3d_draw_lod(int page, const VcpeLodMesh *m, int level);

extern int vcpe_stat_tris;   /* triangles submitted this frame */
extern int vcpe_stat_cull_tests, vcpe_stat_culled, vcpe_stat_cull_tris, vcpe_stat_cull_nodes;

#endif
