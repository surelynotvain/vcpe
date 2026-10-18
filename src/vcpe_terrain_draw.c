/* VCPE terrain on the GE: culling, level choice, cache write-back and the two texture passes (see vcpe_terrain.h). */
#include <pspkernel.h>
#include <math.h>
#include <string.h>
#define VCPE_CULL_WEAK
#include "vcpe_3d.h"
#undef VCPE_CULL_WEAK
#include "vcpe_terrain.h"

int vcpe_terrain_stat_cells, vcpe_terrain_stat_tris, vcpe_terrain_stat_culled;
static uint32_t stat_frame;

static void stats_frame(void)
{
    if (stat_frame != vcpe_gfx_frame) {
        stat_frame = vcpe_gfx_frame;
        vcpe_terrain_stat_cells = vcpe_terrain_stat_tris = vcpe_terrain_stat_culled = 0;
    }
}

static void translate(float m[16], float x, float y, float z)
{
    static const float id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    memcpy(m, id, sizeof id);
    m[3] = x;
    m[7] = y;
    m[11] = z;
}

/* The base pass ignores vertex alpha (it is the overlay's coverage), so the alpha test is off; the overlay pass
 * blends by it at the same depth (the depth test is "greater or equal") without writing depth. The default 3D
 * material (no blending, alpha test above 0x20) is restored afterwards. */
int vcpe_terrain_draw(VcpeTerrainCell *c, const float o[3], float thresh_px, int overlay_level)
{
    if (!c || !c->blob)
        return 0;
    stats_frame();
    VcpeCullBox box;
    box.valid = 1;
    vcpe_terrain_bounds(c, o, box.center, box.extent);
    /* the box when vcpe_cull is linked, else the bounding sphere (vcpe_3d's own frustum test) */
    int seen = vcpe_cull_box_test ? vcpe_3d_visible_box(&box, 0)
                                  : vcpe_3d_visible(box.center, sqrtf(box.extent[0] * box.extent[0] * 2 + box.extent[1] * box.extent[1]));
    if (!seen) {
        vcpe_terrain_stat_culled++;
        return 0;
    }
    VcpeLodView v;
    vcpe_3d_lod_view(&v, thresh_px);   /* camera of vcpe_3d_begin: already relative to the origin */
    int level = vcpe_terrain_pick(c, &v, o, c->level), count;
    c->level = level;
    const uint16_t *idx = vcpe_terrain_indices(c->n, level, &count);
    if (!idx)
        return 0;
    if (c->dirty) {
        sceKernelDcacheWritebackRange(c->verts, (unsigned)c->nverts * sizeof(VcpeVtx));
        c->dirty = 0;
    }
    float m[16];
    translate(m, c->cx * c->size - o[0], -o[1], c->cz * c->size - o[2]);
    vcpe_3d_model(m);
    vcpe_3d_material(0, 0);
    vcpe_3d_draw(c->blob->page, c->verts, idx, count);
    int tris = count / 3;
    if (c->has_overlay && level <= overlay_level) {
        vcpe_3d_material(1, 0);
        vcpe_3d_draw(c->blob->overlay, c->verts, idx, count);
        tris += count / 3;
    }
    vcpe_3d_material(0, 0x20);
    vcpe_3d_model(0);
    vcpe_terrain_stat_cells++;
    vcpe_terrain_stat_tris += tris;
    return tris;
}

int vcpe_terrain_draw_far(VcpeTerrainFar *f, const float o[3])
{
    if (!f || !f->verts || !f->nidx)
        return 0;
    stats_frame();
    if (f->dirty) {
        sceKernelDcacheWritebackRange(f->verts, (unsigned)f->vertex_limit * sizeof(VcpeVtx));
        sceKernelDcacheWritebackRange(f->idx, (unsigned)f->nidx * 2u);
        f->dirty = 0;
    }
    if (f->cut_changed) {
        /* the far grid surrounds the camera, so only its chunks can be culled: refresh them for the new cut (a heap
         * allocation, but only when the loaded area moved) */
        if (vcpe_cull_forget) vcpe_cull_forget(f->verts);
        if (vcpe_3d_autocull_enabled()) vcpe_3d_prepare(f->verts, (uint32_t)f->vertex_limit, f->idx, (uint32_t)f->nidx);
        f->cut_changed = 0;
    }
    float m[16];
    translate(m, f->x0 - o[0], -f->sink - o[1], f->z0 - o[2]);
    vcpe_3d_model(m);
    vcpe_3d_draw(f->page, f->verts, f->idx, f->nidx);
    vcpe_3d_model(0);
    vcpe_terrain_stat_tris += f->nidx / 3;
    return f->nidx / 3;
}
