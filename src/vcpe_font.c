/* VCPE text drawing (see vcpe_font.h). */
#include <string.h>
#include "vcpe_gfx.h"
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
