/* Optional logical 2D canvas adapter. Include after the VCPE headers in a game to scale its drawing calls without
 * changing shared engine files. Sprite view offsets stay in logical units; primitives take logical screen units.
 * Set scale 1 before HUD/menus. Physics and texture coordinates are never scaled. */
#ifndef VCPE_CANVAS_H
#define VCPE_CANVAS_H
#include "vcpe_gfx.h"
#include "vcpe_spr.h"
void vcpe_canvas_scale(float scale);
float vcpe_canvas_get_scale(void);
void vcpe_canvas_view(float x, float y);
void vcpe_canvas_sprite(int img, int frame, float x, float y, float xs, float ys, float angle, uint32_t color);
void vcpe_canvas_part(int img, int frame, int l, int t, int w, int h, float x, float y, float xs, float ys, uint32_t color);
void vcpe_canvas_tiled(int img, int frame, float x, float y, int tx, int ty, float xs, float ys, uint32_t color);
void vcpe_canvas_text(int font, float x, float y, const char *s, int ha, int va, float scale, uint32_t color);
void vcpe_canvas_fill(float x, float y, float w, float h, uint32_t color);
void vcpe_canvas_strip(const float *xy, const uint32_t *colors, int n, int additive);
#ifndef VCPE_CANVAS_NO_ALIASES
#define vcpe_spr_view vcpe_canvas_view
#define vcpe_spr_draw vcpe_canvas_sprite
#define vcpe_spr_draw_part vcpe_canvas_part
#define vcpe_spr_draw_tiled vcpe_canvas_tiled
#define vcpe_spr_text vcpe_canvas_text
#define vcpe_fill vcpe_canvas_fill
#define vcpe_strip vcpe_canvas_strip
#endif
#endif
