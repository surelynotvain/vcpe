/* VCPE 3D: perspective rendering on the GU. A 16-bit depth buffer in VRAM (only once a game asks for 3D),
 * a camera given like a Godot Camera3D (position, YXZ euler angles in degrees, vertical FOV), and indexed
 * meshes in world space: VcpeVtx vertices (float UV, ABGR colour, float position) with 16-bit indices.
 * Draw the 3D scene between vcpe_3d_begin() and vcpe_3d_end(), then 2D (HUD) as before. */
#ifndef VCPE_3D_H
#define VCPE_3D_H
#include <stdint.h>

void vcpe_3d_begin(const float pos[3], const float rot_deg[3], float fov_deg, float znear, float zfar);
void vcpe_3d_draw(int page, const void *verts, const uint16_t *idx, int nidx);   /* page < 0: untextured */
void vcpe_3d_fog(float start, float end, uint32_t color);                         /* end <= start: off */
void vcpe_3d_end(void);

extern int vcpe_stat_tris;   /* triangles submitted this frame */

#endif
