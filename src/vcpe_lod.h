/* VCPE LOD meshes: one mesh with several detail levels (tools/vcpe_lod.py), the level picked per frame by how big
 * its error looks on screen. Far away, neighbouring triangles are joined while the change stays under a pixel or so.
 *
 *   VcpeLodMesh m;  vcpe_lod_open(&m, mem, size);           // mem: the VLD1 blob, kept by the game (PAK, pack)
 *   VcpeLodView view;  vcpe_3d_lod_view(&view, 1.0f);       // after vcpe_3d_begin: the camera, 1 px allowed
 *   lvl = vcpe_lod_pick(&m, &view, centre, scale, lvl);     // pass the previous level back: hysteresis
 *   vcpe_3d_draw_lod(page, &m, lvl);
 *
 * Plain C with no PSP headers: the picking runs anywhere (host tests, other platforms). Only vcpe_3d_draw_lod (in
 * vcpe_3d.c) talks to the GPU. */
#ifndef VCPE_LOD_H
#define VCPE_LOD_H
#include <stdint.h>

#define VCPE_LOD_MAX 8

typedef struct {
    uint16_t nverts;     /* vertices this level uses: always a prefix of the vertex array */
    uint16_t pad;
    uint32_t nidx;       /* u16 indices, 3 per triangle */
    uint32_t idx_off;    /* from the start of the blob */
    float err;           /* largest distance from the full mesh, in model units */
} VcpeLodLevel;

typedef struct {
    const void *verts;   /* VcpeVtx (u, v, ABGR, x, y, z), inside the blob */
    const VcpeLodLevel *lv;
    const uint8_t *base;
    int nverts, nlevels;
    float centre[3], radius;   /* bounding sphere in model units */
} VcpeLodMesh;

/* The camera as the picker needs it: position and pixels per model unit at distance 1 (screen height / 2 / tan of
 * half the vertical FOV). thresh_px: the error allowed on screen; down_k: a coarser level is only taken once its
 * error falls below thresh_px x down_k (0.75 by default), so a level does not flicker at the boundary. */
typedef struct {
    float pos[3];
    float px_k;
    float thresh_px;
    float down_k;
} VcpeLodView;

/* blob must stay in memory and be 4-byte aligned (16 for the GE). 0: ok; -1 not a VLD1 blob; -2 damaged. */
int vcpe_lod_open(VcpeLodMesh *m, const void *blob, int size);
void vcpe_lod_view(VcpeLodView *v, const float cam_pos[3], float fov_deg, int screen_h, float thresh_px);
/* centre: the mesh's bounding-sphere centre in world space (its model matrix applied); scale: the model's largest
 * scale factor. prev: the level used last frame, or -1. Returns the coarsest level whose error stays invisible. */
int vcpe_lod_pick(const VcpeLodMesh *m, const VcpeLodView *v, const float centre[3], float scale, int prev);
float vcpe_lod_error_px(const VcpeLodMesh *m, const VcpeLodView *v, const float centre[3], float scale, int level);
static inline const uint16_t *vcpe_lod_idx(const VcpeLodMesh *m, int l) { return (const uint16_t *)(m->base + m->lv[l].idx_off); }
static inline int vcpe_lod_nidx(const VcpeLodMesh *m, int l) { return (int)m->lv[l].nidx; }

/* frame statistics (perf lines), counted by vcpe_3d_draw_lod and reset by vcpe_3d_begin: triangles the full meshes
 * would have had, and triangles actually sent */
extern int vcpe_lod_stat_full, vcpe_lod_stat_drawn;

#endif
