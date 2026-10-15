/* Visible-tile batching for orthogonal 2D maps. Call once per layer.
 * With disjoint=1, rectangles may be grouped by source image/page; use 0 for
 * overlapping layers (table overhangs, translucent stacks, painter ordering).
 * World coordinates and original image pixels are retained; only GPU vertices
 * are scaled to the physical framebuffer. No full-resolution software canvas. */
#ifndef VCPE_TILES_H
#define VCPE_TILES_H
#include <stdint.h>
void vcpe_tiles_begin(float camera_x,float camera_y,float scale_x,float scale_y,int disjoint);
void vcpe_tiles_part(int image,int left,int top,int w,int h,float world_x,float world_y,uint32_t color);
void vcpe_tiles_end(void);
extern unsigned vcpe_stat_tile_quads,vcpe_stat_tile_culled;
#endif
