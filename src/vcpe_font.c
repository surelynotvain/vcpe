/* VCPE text drawing (see vcpe_font.h). */
#include <string.h>
#include <pspgu.h>
#include <pspgum.h>
#include "vcpe_gfx.h"
#include "vcpe_3d.h"
#include "vcpe_font.h"

static uint32_t next_cp(const char **ps)
{
    const unsigned char *s = (const unsigned char *)*ps;
    uint32_t c = *s++;
    if (c >= 0xF0 && s[0] && s[1] && s[2]) { c = ((c & 7) << 18) | ((s[0] & 63) << 12) | ((s[1] & 63) << 6) | (s[2] & 63); s += 3; }
    else if (c >= 0xE0 && s[0] && s[1]) { c = ((c & 15) << 12) | ((s[0] & 63) << 6) | (s[1] & 63); s += 2; }
    else if (c >= 0xC0 && s[0]) { c = ((c & 31) << 6) | (s[0] & 63); s += 1; }
    *ps = (const char *)s;
    return c;
}

int vcpe_utf8_len(const char *s)
{
    int n = 0;
    while (*s) {
        next_cp(&s);
        n++;
    }
    return n;
}

static const VcpeGlyph *glyph(const VcpeFont *f, uint32_t cp)
{
    int lo = 0, hi = f->nglyphs - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (f->glyphs[m].cp == cp) return &f->glyphs[m];
        if (f->glyphs[m].cp < cp) lo = m + 1; else hi = m - 1;
    }
    return 0;
}

static float line_width(const VcpeFont *f, const char *s, int *left, float scale)
{
    float w = 0;
    while (*s && *s != '\n' && *left != 0) {
        const VcpeGlyph *g = glyph(f, next_cp(&s));
        if (g) w += g->adv * scale;
        if (*left > 0) (*left)--;
    }
    return w;
}

float vcpe_font_width(const VcpeFont *f, const char *s, int max_chars, float scale)
{
    int left = max_chars < 0 ? -1 : max_chars;
    return line_width(f, s, &left, scale);
}

static const char *draw_line(const VcpeFont *f, float x, float y, const char *s, int *left, uint32_t color, float scale)
{
    while (*s && *s != '\n') {
        uint32_t cp = next_cp(&s);
        if (*left == 0) continue;
        if (*left > 0) (*left)--;
        const VcpeGlyph *g = glyph(f, cp);
        if (!g) continue;
        if (g->w && g->h)
            vcpe_quad(x + g->xoff * scale, y + g->yoff * scale, x + (g->xoff + g->w) * scale, y + (g->yoff + g->h) * scale,
                      g->u, g->v, g->u + g->w, g->v + g->h, color);
        x += g->adv * scale;
    }
    return s;
}

void vcpe_font_draw(const VcpeFont *f, float x, float y, const char *s, int max_chars, uint32_t color, float scale)
{
    if (!vcpe_bind_page(f->page)) return;
    vcpe_tex_white(1);
    int left = max_chars < 0 ? -1 : max_chars;
    while (*s) {
        s = draw_line(f, x, y, s, &left, color, scale);
        if (*s == '\n') { s++; if (left > 0) left--; }
        y += f->height * scale;
    }
    vcpe_tex_white(0);
}

void vcpe_font_draw_centered(const VcpeFont *f, float cx, float cy, const char *s, int max_chars, uint32_t color, float scale)
{
    if (!vcpe_bind_page(f->page)) return;
    vcpe_tex_white(1);
    int lines = 1;
    for (const char *p = s; *p; p++) lines += *p == '\n';
    float y = cy - lines * f->height * scale * 0.5f;
    int left = max_chars < 0 ? -1 : max_chars;
    while (*s) {
        int probe = -1;
        float w = line_width(f, s, &probe, scale);
        s = draw_line(f, cx - w * 0.5f, y, s, &left, color, scale);
        if (*s == '\n') { s++; if (left > 0) left--; }
        y += f->height * scale;
    }
    vcpe_tex_white(0);
}

void vcpe_font_draw3d(const VcpeFont *f, const float m[16], const char *s, float size, uint32_t color, int align)
{
    int nchars = vcpe_utf8_len(s);
    if (!nchars || !vcpe_bind_page(f->page))
        return;
    int lines = 1;
    for (const char *p = s; *p; p++) lines += *p == '\n';
    int pw, ph;
    vcpe_page_size(f->page, &pw, &ph);
    float su = 1.0f / pw, sv = 1.0f / ph;   /* 3D drawing takes normalised texture coordinates */
    VcpeVtx *v = vcpe_gfx_mem(sizeof(VcpeVtx) * 6 * nchars);
    if (!v)
        return;
    int n = 0;
    float y = lines * f->height * 0.5f;           /* top of the first line, in baked pixels (y up) */
    const char *p = s;
    while (*p) {
        int probe = -1;
        float w = line_width(f, p, &probe, 1.0f);
        float x = align == 1 ? -w * 0.5f : align == 2 ? -w : 0;
        while (*p && *p != '\n') {
            const VcpeGlyph *g = glyph(f, next_cp(&p));
            if (!g) continue;
            if (g->w && g->h) {
                float x0 = (x + g->xoff) * size, x1 = (x + g->xoff + g->w) * size;
                float y0 = (y - g->yoff) * size, y1 = (y - g->yoff - g->h) * size;
                float u0 = g->u * su, u1 = (g->u + g->w) * su, v0 = g->v * sv, v1 = (g->v + g->h) * sv;
                VcpeVtx q[6] = {{u0, v0, color, x0, y0, 0}, {u1, v0, color, x1, y0, 0}, {u1, v1, color, x1, y1, 0},
                                {u0, v0, color, x0, y0, 0}, {u1, v1, color, x1, y1, 0}, {u0, v1, color, x0, y1, 0}};
                memcpy(v + n, q, sizeof q);
                n += 6;
            }
            x += g->adv;
        }
        if (*p == '\n') p++;
        y -= f->height;
    }
    if (!n) return;
    vcpe_gfx_flush();
    vcpe_3d_model(m);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuDepthMask(GU_TRUE);
    vcpe_tex_white(1);
    vcpe_filter(0);
    sceGumDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, n, 0, v);
    vcpe_tex_white(0);
    vcpe_filter(1);
    sceGuDepthMask(GU_FALSE);
    sceGuDisable(GU_BLEND);
    vcpe_3d_model(0);
}
