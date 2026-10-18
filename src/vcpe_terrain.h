/* VCPE terrain: open-world ground as a grid of heightfield cells (tools/vcpe_terrain.py cuts them from one big
 * heightmap). Each cell is n x n quads (n a power of two, up to 64) with geomipmapped detail levels that share one
 * vertex array, skirts that hide the cracks between neighbours drawn at different levels, an optional second
 * texture painted over the first by vertex alpha (roads, snow, rock), exact height/normal queries for walking and
 * foot IK, and CPU relighting for a moving sun. Vertices are stored relative to the cell's corner and drawn with a
 * translation, so precision does not degrade far from the world origin (see vcpe_cells for origin shifts).
 *
 *   VcpeTerrainCell c;  vcpe_terrain_cell(&c, blob, size, mem, cap);   // mem: vcpe_terrain_cell_bytes(blob)
 *   VcpeTerrainLight l = {{0.4f, 0.8f, 0.3f}, 0xffd0e0ff, 0xff504040};
 *   vcpe_terrain_relight(&c, &l);                                         // (or vcpe_terrain_relight_some per frame)
 *   vcpe_terrain_height(&c, x, z, &y, normal);                            // world x, z (cell origin + local)
 *   in 3D: vcpe_terrain_draw(&c, origin, &view, 1.5f);                    // vcpe_terrain_draw.c, picks the level
 *
 * This file and vcpe_terrain.c are plain C (host tests); only vcpe_terrain_draw.c talks to the GE.
 * Axes: X east, Y up, Z south (vcpe_3d); sample (i, j) is column i (x), row j (z). */
#ifndef VCPE_TERRAIN_H
#define VCPE_TERRAIN_H
#include <stdint.h>
#include "vcpe_gfx.h"
#include "vcpe_lod.h"

#define VCPE_TERRAIN_MAX_N 64
#define VCPE_TERRAIN_LEVELS 6      /* steps 1, 2, 4 ... 32 */

/* VTR1 cell blob (little endian, 16-byte aligned):
 *   header  "VTR1", u16 n, u16 flags, i32 cell x, i32 cell z, float size (metres per side), float height base,
 *           float height step, i16 page, i16 overlay page (-1 none), float uv repeats per cell, u32 colour offset
 *           (0: white), u32 height offset, u32 normal offset (0: from the heights)                      (48 bytes)
 *   heights u16[(n+1)^2], row by row: height = base + q x step
 *   colours u32[(n+1)^2] ABGR (optional); alpha = how much of the overlay texture covers the vertex
 *   normals i8[(n+1)^2][4] x y z 0, scaled to 127 (optional; the cutter computes them across cell borders, so
 *           lighting has no seams; without them border normals are one-sided) */
typedef struct {
    char magic[4];
    uint16_t n, flags;
    int32_t cx, cz;
    float size, base, step;
    int16_t page, overlay;
    float uv_repeat;
    uint32_t colors, heights, normals;
} VcpeTerrainBlob;                 /* 48 bytes */

typedef struct {
    const VcpeTerrainBlob *blob;
    const uint16_t *h;             /* heights, in the blob */
    const uint32_t *base_color;    /* in the blob, or 0 (white) */
    VcpeVtx *verts;                /* (n+1)^2 grid vertices, then 4 (n+1) skirt vertices; positions cell-local */
    int8_t (*normal)[4];           /* per grid vertex, x y z scaled to 127 */
    int n, nverts, cx, cz;
    float size, quad, ymin, ymax;  /* quad: metres per quad; bounds of the heights */
    float err[VCPE_TERRAIN_LEVELS];/* largest height error of each level, metres (level 0: 0) */
    int levels;
    int level;                     /* last level drawn, for hysteresis */
    uint32_t light_serial;         /* the light the colours were computed for */
    uint8_t dirty;                 /* vertices changed since the GE last saw them (vcpe_terrain_draw writes back) */
    uint8_t has_overlay;           /* an overlay page and some vertex with alpha > 0 */
    float skirt;                   /* metres the skirts hang below the edges */
} VcpeTerrainCell;

typedef struct {
    float sun[3];                  /* toward the sun (normalised by the module) */
    uint32_t sun_color, ambient;   /* ABGR; lit = base x (ambient + sun_color x max(0, n . sun)) */
    uint32_t serial;               /* change it whenever the light changes (vcpe_sky does) */
} VcpeTerrainLight;

/* bytes vcpe_terrain_cell needs for this blob (vertices and normals), 0 when it is not a valid VTR1 blob */
uint32_t vcpe_terrain_cell_bytes(const void *blob, uint32_t size);
/* Build a cell over a blob the game keeps loaded. mem: 16-byte aligned, at least cell_bytes. 0 ok, -1 not VTR1,
 * -2 damaged or too small memory. Colours start unlit (base colours) until relit. */
int vcpe_terrain_cell(VcpeTerrainCell *c, const void *blob, uint32_t size, void *mem, uint32_t cap);

/* World x, z -> height and normal (either may be 0), on the same triangles level 0 draws. 0 outside the cell. */
int vcpe_terrain_height(const VcpeTerrainCell *c, float x, float z, float *y, float normal[3]);
/* Same over many cells: the first that contains the point */
int vcpe_terrain_height_any(VcpeTerrainCell *const *cells, int count, float x, float z, float *y, float normal[3]);
/* first ground hit along a world-space ray within max_dist (stepping by half a quad, refined by bisection) */
int vcpe_terrain_ray(const VcpeTerrainCell *c, const float origin[3], const float dir[3], float max_dist, float *hit_t);

/* Recolour every vertex for this light (about 2.5 us per vertex on a PSP). */
void vcpe_terrain_relight(VcpeTerrainCell *c, const VcpeTerrainLight *l);
/* Spread relighting over frames: relights cells whose colours are older than the light, at most max_vertices
 * vertices per call (whole cells; at least one). Returns cells relit. */
int vcpe_terrain_relight_some(VcpeTerrainCell *const *cells, int count, const VcpeTerrainLight *l, int max_vertices);

/* Index lists shared by every cell of side n: level 0 = every quad. With skirts appended. */
const uint16_t *vcpe_terrain_indices(int n, int level, int *count);
/* Level for a camera (vcpe_lod rule: screen error under thresh_px pixels, hysteresis on the previous level) */
int vcpe_terrain_pick(const VcpeTerrainCell *c, const VcpeLodView *v, const float origin[3], int prev);
/* the cell's box in world space relative to origin (what is drawn at the model translation) */
void vcpe_terrain_bounds(const VcpeTerrainCell *c, const float origin[3], float center[3], float extent[3]);

/* Far terrain: one coarse grid over the whole world (a few samples per cell) drawn beyond the loaded cells, with
 * the quads under loaded cells left out. Heights u16 like a cell; built into game memory; rebuilt (cheaply) when
 * the loaded area moves. */
typedef struct {
    const uint16_t *h;             /* (w+1) x (d+1) samples, kept by the game */
    int w, d;                      /* quads */
    float x0, z0, quad, base, step;/* world position of sample (0, 0), metres per quad, height decoding */
    uint32_t color;                /* ABGR base colour */
    int16_t page;
    VcpeVtx *verts;                /* (w+1)(d+1) */
    uint16_t *idx;                 /* up to 6 w d */
    int nidx, vertex_limit;
    float sink;                    /* metres the far grid is lowered so it never pokes through nearer cells */
    uint32_t light_serial;
    uint8_t dirty, cut_changed;    /* GE write-back / cull cache refresh pending (vcpe_terrain_draw_far) */
} VcpeTerrainFar;
uint32_t vcpe_terrain_far_bytes(int w, int d);
/* mem 16-byte aligned, far_bytes(w, d). w, d: (w+1)(d+1) <= 65535. -1 bad input */
int vcpe_terrain_far_init(VcpeTerrainFar *f, const uint16_t *heights, int w, int d, float x0, float z0, float quad,
                          float base, float step, int16_t page, uint32_t color, void *mem);
/* Leave out quads whose centre lies inside the world rectangle (the loaded cells); x1 <= x0 keeps everything.
 * Relit with the light like a cell. Returns triangles kept. */
int vcpe_terrain_far_cut(VcpeTerrainFar *f, float hx0, float hz0, float hx1, float hz1);
void vcpe_terrain_far_relight(VcpeTerrainFar *f, const VcpeTerrainLight *l);

/* GE side (vcpe_terrain_draw.c, needs vcpe_3d): inside vcpe_3d_begin/end. origin: the world position that sits at
 * the camera space origin (vcpe_cells_origin), so the model matrix is a small translation. Returns triangles drawn
 * (0 culled). thresh_px: allowed screen error for the level (1..4). Overlays are drawn up to overlay_level. */
int vcpe_terrain_draw(VcpeTerrainCell *c, const float origin[3], float thresh_px, int overlay_level);
int vcpe_terrain_draw_far(VcpeTerrainFar *f, const float origin[3]);
extern int vcpe_terrain_stat_cells, vcpe_terrain_stat_tris, vcpe_terrain_stat_culled;   /* this frame (vcpe_gfx_frame) */

#endif
