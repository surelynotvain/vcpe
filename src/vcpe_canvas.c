#define VCPE_CANVAS_NO_ALIASES
#include "vcpe_canvas.h"

static float zoom = 1, view_x, view_y;
float vcpe_canvas_get_scale(void) { return zoom; }
void vcpe_canvas_view(float x, float y)
{
    view_x = x; view_y = y;
    vcpe_spr_view(zoom == 1 ? x : 0, zoom == 1 ? y : 0);
}
void vcpe_canvas_scale(float scale)
{
    zoom = scale > 0 ? scale : 1;
    vcpe_canvas_view(view_x, view_y);
}
void vcpe_canvas_sprite(int img, int frame, float x, float y, float xs, float ys, float angle, uint32_t color)
{
    if (zoom != 1) { x = (x - view_x) * zoom; y = (y - view_y) * zoom; xs *= zoom; ys *= zoom; }
    vcpe_spr_draw(img, frame, x, y, xs, ys, angle, color);
}
void vcpe_canvas_part(int img, int frame, int l, int t, int w, int h, float x, float y, float xs, float ys, uint32_t color)
{
    if (zoom != 1) { x = (x - view_x) * zoom; y = (y - view_y) * zoom; xs *= zoom; ys *= zoom; }
    vcpe_spr_draw_part(img, frame, l, t, w, h, x, y, xs, ys, color);
}
void vcpe_canvas_tiled(int img, int frame, float x, float y, int tx, int ty, float xs, float ys, uint32_t color)
{
    if (zoom != 1) { x = (x - view_x) * zoom; y = (y - view_y) * zoom; xs *= zoom; ys *= zoom; }
    vcpe_spr_draw_tiled(img, frame, x, y, tx, ty, xs, ys, color);
}
void vcpe_canvas_text(int font, float x, float y, const char *s, int ha, int va, float scale, uint32_t color)
{
    if (zoom != 1) { x = (x - view_x) * zoom; y = (y - view_y) * zoom; scale *= zoom; }
    vcpe_spr_text(font, x, y, s, ha, va, scale, color);
}
void vcpe_canvas_fill(float x, float y, float w, float h, uint32_t color)
{
    vcpe_fill(x * zoom, y * zoom, w * zoom, h * zoom, color);
}
void vcpe_canvas_strip(const float *xy, const uint32_t *colors, int n, int additive)
{
    if (zoom == 1) { vcpe_strip(xy, colors, n, additive); return; }
    /* Overlap the last two vertices to retain strip connectivity. Even-sized advances retain winding too. */
    for (int start = 0; start < n - 2; start += 62) {
        float scaled[128];
        int count = n - start < 64 ? n - start : 64;
        for (int k = 0; k < count * 2; k++) scaled[k] = xy[start * 2 + k] * zoom;
        vcpe_strip(scaled, colors + start, count, additive);
    }
}
