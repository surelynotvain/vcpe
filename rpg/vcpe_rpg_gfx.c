/* VCPE RPG graphics: the project's virtual screen on the display, RPG Maker's window skin, faces, icons, gauges and
 * text with escape codes.
 *
 * The project draws in its own pixels (Graphics.width x Graphics.height); rpg_view(scale, ox, oy) says where they
 * land on the display. Pictures can have their own mapping (rpg_picture_xform), e.g. full-screen artwork fitted to
 * the display height while the map and windows use another scale.
 *
 * Window skin layout (VX Ace, 128 x 128): background 0,0 64x64 (stretched) with a tiled pattern 0,64 64x64 over it,
 * frame 64,0 64x64 (16 px corners), cursor 64,64 32x32 (8 px borders), pause sign 96,64 2x2 frames of 16x16, text
 * colours as 8x8 squares at 64,96 (8 per row, 32 colours; the converter reads them into a table). */
#include "vcpe_rpg.h"
#include "vcpe_spr.h"
#include "vcpe_gfx.h"
#include "vcpe_ttf.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

float rpg_scale = 1, rpg_ox, rpg_oy;
int rpg_screen_w = 544, rpg_screen_h = 416;
RpgXform rpg_picture_xform = {1, 0, 0};
float rpg_font_size = 24;
int rpg_font_outline = 1;
static VcpeTtf *font;
static int skin = -1, iconset = -2;
static uint32_t skin_colors[32];

void rpg_view(float scale, float ox, float oy, int w, int h)
{
    rpg_scale = scale;
    rpg_ox = ox;
    rpg_oy = oy;
    rpg_screen_w = w;
    rpg_screen_h = h;
    rpg_picture_xform = (RpgXform){scale, ox, oy};
}

uint32_t rpg_rgba(int r, int g, int b, int a)
{
#define C8(v) ((uint32_t)((v) < 0 ? 0 : (v) > 255 ? 255 : (v)))
    return C8(r) | C8(g) << 8 | C8(b) << 16 | C8(a) << 24;
}

/* images may be stored smaller than the original (rpg_image_scale): sizes and source rectangles stay in original
 * pixels everywhere, the conversion happens here */
float (*rpg_image_scale)(int img);
static float img_scale(int img) { return rpg_image_scale ? rpg_image_scale(img) : 1.f; }

int rpg_image_w(int img)
{
    const VcpeSprImage *im = img >= 0 ? vcpe_spr_image(img) : 0;
    return im ? (int)(im->w / img_scale(img) + 0.5f) : 0;
}
int rpg_image_h(int img)
{
    const VcpeSprImage *im = img >= 0 ? vcpe_spr_image(img) : 0;
    return im ? (int)(im->h / img_scale(img) + 0.5f) : 0;
}

void rpg_draw_part_x(const RpgXform *xf, int img, int sx, int sy, int sw, int sh, float x, float y, float zx,
                     float zy, float angle, uint32_t c, int blend)
{
    if (img < 0 || sw <= 0 || sh <= 0 || !(c >> 24))
        return;
    float is = img_scale(img);
    if (is != 1.f) {
        /* the stored image is smaller: its rectangle shrinks, the zoom grows; positions stay */
        int sx2 = (int)(sx * is + 0.5f), sy2 = (int)(sy * is + 0.5f);
        int sw2 = (int)((sx + sw) * is + 0.5f) - sx2, sh2 = (int)((sy + sh) * is + 0.5f) - sy2;
        zx *= (float)sw / (sw2 > 0 ? sw2 : 1);
        zy *= (float)sh / (sh2 > 0 ? sh2 : 1);
        sx = sx2;
        sy = sy2;
        sw = sw2;
        sh = sh2;
    }
    vcpe_spr_additive(blend);
    float s = xf->scale;
    if (angle == 0) {
        vcpe_spr_draw_part(img, 0, sx, sy, sw, sh, xf->ox + x * s, xf->oy + y * s, zx * s, zy * s, c);
    } else {
        /* rotation about (x, y): vcpe_spr_draw_part_ex pivots on the part's centre, so place that centre where the
         * rotation takes it (counter-clockwise on screen, as RPG Maker's Sprite#angle) */
        float hw = sw * zx * 0.5f, hh = sh * zy * 0.5f;
        float r = angle * 3.14159265f / 180.f, cs = cosf(r), sn = sinf(r);
        float cx = x + hw * cs + hh * sn, cy = y - hw * sn + hh * cs;
        vcpe_spr_draw_part_ex(img, 0, sx, sy, sw, sh, xf->ox + cx * s, xf->oy + cy * s, zx * s, zy * s, angle, c);
    }
    if (blend)
        vcpe_spr_additive(0);
}

void rpg_draw_part(int img, int sx, int sy, int sw, int sh, float x, float y, float zx, float zy, float angle,
                   uint32_t c, int blend)
{
    RpgXform xf = {rpg_scale, rpg_ox, rpg_oy};
    rpg_draw_part_x(&xf, img, sx, sy, sw, sh, x, y, zx, zy, angle, c, blend);
}

void rpg_fill(float x, float y, float w, float h, uint32_t c)
{
    if (!(c >> 24) || w <= 0 || h <= 0)
        return;
    float x0 = floorf(rpg_ox + x * rpg_scale + 0.5f), y0 = floorf(rpg_oy + y * rpg_scale + 0.5f);
    float x1 = floorf(rpg_ox + (x + w) * rpg_scale + 0.5f), y1 = floorf(rpg_oy + (y + h) * rpg_scale + 0.5f);
    vcpe_fill(x0, y0, x1 - x0, y1 - y0, c);
}

void rpg_clip(float x, float y, float w, float h)
{
    int x0 = (int)floorf(rpg_ox + x * rpg_scale), y0 = (int)floorf(rpg_oy + y * rpg_scale);
    int x1 = (int)ceilf(rpg_ox + (x + w) * rpg_scale), y1 = (int)ceilf(rpg_oy + (y + h) * rpg_scale);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > VCPE_SCR_W) x1 = VCPE_SCR_W;
    if (y1 > VCPE_SCR_H) y1 = VCPE_SCR_H;
    vcpe_clip(x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0);
}

void rpg_clip_off(void) { vcpe_clip_reset(); }

/* A Tone over a rectangle, as the GE can do it without render targets: negative channels darken (multiply by
 * (255 + t) / 255 through subtractive blending of the colour that is taken away), positive ones brighten (additive),
 * gray desaturates approximately by washing towards the mean (drawn as gray at gray/255 alpha). */
void rpg_screen_tone(const float t[4], float x, float y, float w, float h)
{
    int r = (int)t[0], g = (int)t[1], b = (int)t[2], gray = (int)t[3];
    if (gray > 0)
        rpg_fill(x, y, w, h, rpg_rgba(128, 128, 128, gray / 2));
    int nr = r < 0 ? -r : 0, ng = g < 0 ? -g : 0, nb = b < 0 ? -b : 0;
    if (nr || ng || nb) {
        vcpe_blend(2);
        rpg_fill(x, y, w, h, rpg_rgba(nr, ng, nb, 255));
        vcpe_blend(0);
    }
    int pr = r > 0 ? r : 0, pg = g > 0 ? g : 0, pb = b > 0 ? b : 0;
    if (pr || pg || pb) {
        vcpe_blend(1);
        rpg_fill(x, y, w, h, rpg_rgba(pr, pg, pb, 255));
        vcpe_blend(0);
    }
}

/* ------------------------------------------------------------------ text */
void rpg_font(VcpeTtf *f, float size, int outline)
{
    font = f;
    rpg_font_size = size;
    rpg_font_outline = outline;
}

float rpg_line_height(void) { return rpg_font_size + 2 > 24 ? rpg_font_size + 2 : 24; }

/* U+3000 (ideographic space): drawn with the font's own glyph when it has one (its advance is what RGSS uses, and
 * messages centre announcements with runs of them); fonts without it would draw '?': ASCII spaces of about an em */
static const char *ideo_spaces(const char *s, char *buf, int cap)
{
    if (!strstr(s, "\xe3\x80\x80") || vcpe_ttf_has(font, 0x3000))
        return s;
    float sp = vcpe_ttf_width(font, rpg_font_size * rpg_scale, " ");
    int k = sp > 0 ? (int)(rpg_font_size * rpg_scale / sp + 0.5f) : 3, n = 0;
    while (*s && n < cap - 8) {
        if (!strncmp(s, "\xe3\x80\x80", 3)) {
            for (int i = 0; i < k && n < cap - 1; i++)
                buf[n++] = ' ';
            s += 3;
        } else {
            buf[n++] = *s++;
        }
    }
    buf[n] = 0;
    return buf;
}

float rpg_text_width(const char *s)
{
    if (!font || !s)
        return 0;
    char buf[1024];
    s = ideo_spaces(s, buf, sizeof buf);
    return vcpe_ttf_width(font, rpg_font_size * rpg_scale, s) / rpg_scale;
}

/* one line at the top-left (x, y) of a line box of rpg_line_height: RPG Maker centres the glyphs vertically */
float rpg_text(float x, float y, const char *s, uint32_t c)
{
    if (!font || !s || !*s)
        return 0;
    char buf[1024];
    s = ideo_spaces(s, buf, sizeof buf);
    float px = rpg_font_size * rpg_scale;
    float asc = vcpe_ttf_ascent(font, px), desc = vcpe_ttf_descent(font, px);
    float lh = rpg_line_height() * rpg_scale;
    float sx = rpg_ox + x * rpg_scale, sy = rpg_oy + y * rpg_scale + (lh - (asc - desc)) * 0.5f + asc;
    if (rpg_font_outline) {
        uint32_t oc = ((c >> 24) * 128 / 255) << 24;   /* black at half the text's alpha, as RGSS3 */
        vcpe_ttf_draw(font, px, sx - 1, sy, s, oc);
        vcpe_ttf_draw(font, px, sx + 1, sy, s, oc);
        vcpe_ttf_draw(font, px, sx, sy - 1, s, oc);
        vcpe_ttf_draw(font, px, sx, sy + 1, s, oc);
    }
    return vcpe_ttf_draw(font, px, sx, sy, s, c) / rpg_scale;
}

void rpg_text_box(float x, float y, float w, float h, const char *s, uint32_t c, int align)
{
    float tw = rpg_text_width(s);
    float lx = align == 1 ? x + (w - tw) / 2 : align == 2 ? x + w - tw : x;
    rpg_text(lx, y + (h - rpg_line_height()) / 2, s, c);
}

/* ------------------------------------------------------------------ window skin */
void rpg_windowskin(int img) { skin = img; }
void rpg_windowskin_colors(const uint32_t *c32)
{
    memcpy(skin_colors, c32, sizeof skin_colors);
}
uint32_t rpg_text_color(int n) { return skin_colors[n & 31] ? skin_colors[n & 31] : 0xffffffff; }

static void slice(int img, int sx, int sy, int sw, int sh, float x, float y, float w, float h, int b, uint32_t c,
                  int centre)
{
    /* 9-slice with a border of b source pixels */
    float bx = b, by = b;
    if (w < 2 * bx) bx = w / 2;
    if (h < 2 * by) by = h / 2;
    float xs[3] = {x, x + bx, x + w - bx}, ws[3] = {bx, w - 2 * bx, bx};
    float ys[3] = {y, y + by, y + h - by}, hs[3] = {by, h - 2 * by, by};
    int us[3] = {sx, sx + b, sx + sw - b}, uw[3] = {b, sw - 2 * b, b};
    int vs[3] = {sy, sy + b, sy + sh - b}, vh[3] = {b, sh - 2 * b, b};
    for (int j = 0; j < 3; j++)
        for (int i = 0; i < 3; i++)
            if (ws[i] > 0 && hs[j] > 0 && (centre || i != 1 || j != 1))
                rpg_draw_part(img, us[i], vs[j], uw[i], vh[j], xs[i], ys[j], ws[i] / uw[i], hs[j] / vh[j], 0, c, 0);
}

void rpg_window(float x, float y, float w, float h, int opacity, int back_opacity, float openness, const float tone[4])
{
    if (openness <= 0 || skin < 0)
        return;
    if (openness < 1) {
        float nh = h * openness;
        y += (h - nh) / 2;
        h = nh;
    }
    int a = back_opacity * opacity / 255;
    if (a > 0) {
        /* background + pattern, multiplied by the tone's darkening, brightened additively for positive tones */
        float tr = tone ? tone[0] : 0, tg = tone ? tone[1] : 0, tb = tone ? tone[2] : 0;
        uint32_t c = rpg_rgba(255 + (tr < 0 ? (int)tr : 0), 255 + (tg < 0 ? (int)tg : 0), 255 + (tb < 0 ? (int)tb : 0), a);
        rpg_draw_part(skin, 0, 0, 64, 64, x + 2, y + 2, (w - 4) / 64.f, (h - 4) / 64.f, 0, c, 0);
        rpg_clip(x + 2, y + 2, w - 4, h - 4);
        for (float py = y + 2; py < y + h - 2; py += 64)
            for (float px = x + 2; px < x + w - 2; px += 64)
                rpg_draw_part(skin, 0, 64, 64, 64, px, py, 1, 1, 0, c, 0);
        rpg_clip_off();
        if (tr > 0 || tg > 0 || tb > 0) {
            vcpe_blend(1);
            rpg_fill(x + 2, y + 2, w - 4, h - 4,
                     rpg_rgba(tr > 0 ? (int)tr : 0, tg > 0 ? (int)tg : 0, tb > 0 ? (int)tb : 0, a));
            vcpe_blend(0);
        }
    }
    if (opacity > 0)
        slice(skin, 64, 0, 64, 64, x, y, w, h, 16, rpg_rgba(255, 255, 255, opacity), 0);   /* the centre holds the arrows */
}

void rpg_cursor(float x, float y, float w, float h, int frame, int active)
{
    if (skin < 0 || w <= 0 || h <= 0)
        return;
    /* Window#cursor_rect blinks between full and half opacity over 64 frames while active */
    int f = frame % 64;
    int a = active ? 255 - (f < 32 ? f : 64 - f) * 4 : 160;
    slice(skin, 64, 64, 32, 32, x, y, w, h, 8, rpg_rgba(255, 255, 255, a), 1);
}

void rpg_pause_sign(float x, float y, int frame)
{
    if (skin < 0)
        return;
    int f = (frame / 8) % 4;
    rpg_draw_part(skin, 96 + (f % 2) * 16, 64 + (f / 2) * 16, 16, 16, x, y, 1, 1, 0, 0xffffffff, 0);
}

void rpg_icon(int index, float x, float y, int enabled)
{
    if (iconset == -2)
        iconset = rpg_image("system", "IconSet");
    if (iconset < 0 || index <= 0)
        return;
    rpg_draw_part(iconset, index % 16 * 24, index / 16 * 24, 24, 24, x, y, 1, 1, 0,
                  enabled ? 0xffffffff : 0x80ffffff, 0);
}

void rpg_face(const char *name, int index, float x, float y, float size, int enabled)
{
    int img = rpg_image("faces", name);
    if (img < 0)
        return;
    int cw = 96, ch = 96;
    if (name[0] == '$') {
        cw = rpg_image_w(img);
        ch = rpg_image_h(img);
        index = 0;
    }
    float s = size / 96.f;
    vcpe_spr_filter(0);
    rpg_draw_part(img, index % 4 * cw, index / 4 * ch, cw, ch, x, y, s, s, 0, enabled ? 0xffffffff : 0x80ffffff, 0);
    vcpe_spr_filter(1);
}

void rpg_gauge(float x, float y, float w, float rate, uint32_t c1, uint32_t c2)
{
    /* Window_Base#draw_gauge: 6 px high bar at the bottom of a line, background colour 19 */
    if (rate < 0) rate = 0;
    if (rate > 1) rate = 1;
    float gy = y + rpg_line_height() - 8;
    rpg_fill(x, gy, w, 6, rpg_text_color(19));
    float fw = w * rate;
    if (fw <= 0)
        return;
    /* horizontal gradient c1 -> c2 as a few steps */
    int steps = 8;
    for (int i = 0; i < steps; i++) {
        float t = (i + 0.5f) / steps;
        uint32_t c = 0xff000000;
        for (int k = 0; k < 3; k++) {
            int a = (c1 >> (k * 8)) & 255, b = (c2 >> (k * 8)) & 255;
            c |= (uint32_t)(a + (b - a) * t) << (k * 8);
        }
        float sx = x + fw * i / steps, sw = fw / steps + 0.5f;
        rpg_fill(sx, gy, sw, 6, c);
    }
}

/* ------------------------------------------------------------------ escape codes */
static uint32_t utf8_next(const char **p)
{
    const unsigned char *s = (const unsigned char *)*p;
    uint32_t c = *s++;
    if (c >= 0xc0) {
        int n = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : 1;
        c &= 0x3f >> n;
        while (n-- && (*s & 0xc0) == 0x80)
            c = (c << 6) | (*s++ & 0x3f);
    }
    *p = (const char *)s;
    return c;
}

static int bracket(const char **p)
{
    const char *s = *p;
    if (*s != '[')
        return 0;
    int v = atoi(s + 1);
    while (*s && *s != ']')
        s++;
    if (*s)
        s++;
    *p = s;
    return v;
}

/* Window_Base#convert_escape_characters: \\ -> \e, then \V[n] (twice, nested), \N[n], \P[n], \G */
void rpg_convert_escapes(char *out, int cap, const char *in)
{
    char a[4096], b[4096];
    /* every backslash becomes ESC, then ESC ESC is a literal backslash (RGSS3 does the same two gsubs) */
    int n = 0;
    for (const char *s = in; *s && n < (int)sizeof a - 1; s++) {
        if (*s == '\\' && s[1] == '\\') {
            a[n++] = '\\';
            s++;
        } else {
            a[n++] = *s == '\\' ? 0x1b : *s;
        }
    }
    a[n] = 0;
    for (int pass = 0; pass < 2; pass++) {
        n = 0;
        const char *s = a;
        while (*s && n < (int)sizeof b - 64) {
            if (s[0] == 0x1b && (s[1] == 'V' || s[1] == 'v') && s[2] == '[') {
                s += 2;
                int id = bracket(&s);
                n += snprintf(b + n, sizeof b - n, "%d", (int)rpg_var(id));
            } else {
                b[n++] = *s++;
            }
        }
        b[n] = 0;
        memcpy(a, b, n + 1);
    }
    n = 0;
    const char *s = a;
    while (*s && n < cap - 1) {
        if (s[0] == 0x1b && (s[1] == 'N' || s[1] == 'n') && s[2] == '[') {
            s += 2;
            int id = bracket(&s);
            const char *nm = (id > 0 && id < RPG_MAX_ACTORS) ? rpg.actors[id].name : "";
            n += snprintf(out + n, cap - n, "%s", nm);
        } else if (s[0] == 0x1b && (s[1] == 'P' || s[1] == 'p') && s[2] == '[') {
            s += 2;
            int id = bracket(&s);
            int aid = (id >= 1 && id <= rpg.party.nmembers) ? rpg.party.members[id - 1] : 0;
            n += snprintf(out + n, cap - n, "%s", aid ? rpg.actors[aid].name : "");
        } else if (s[0] == 0x1b && (s[1] == 'G' || s[1] == 'g')) {
            s += 2;
            n += snprintf(out + n, cap - n, "%s", RPG_STR(rpg_system()->currency_unit));
        } else {
            out[n++] = *s++;
        }
        if (n >= cap)
            n = cap - 1;
    }
    out[n] = 0;
}

/* draw converted text (escapes as \x1b codes): \C[n] colour, \I[n] icon, \{ \} size; returns the end x */
float rpg_draw_text_ex(float x, float y, const char *t)
{
    float saved = rpg_font_size;
    float x0 = x;
    uint32_t color = rpg_text_color(0);
    char run[512];
    int rn = 0;
    float lh = rpg_line_height();
#define FLUSH() do { if (rn) { run[rn] = 0; x += rpg_text(x, y, run, color); rn = 0; } } while (0)
    for (const char *s = t; *s;) {
        if (*s == '\n') {
            FLUSH();
            s++;
            x = x0;
            y += lh;
            continue;
        }
        if (*s == 0x1b) {
            FLUSH();
            char code = s[1];
            s += 2;
            if (code == 'C' || code == 'c') {
                color = rpg_text_color(bracket(&s));
            } else if (code == 'I' || code == 'i') {
                rpg_icon(bracket(&s), x, y, 1);
                x += 24;
            } else if (code == '{') {
                if (rpg_font_size <= 64) rpg_font_size += 8;
            } else if (code == '}') {
                if (rpg_font_size >= 16) rpg_font_size -= 8;
            } else if (code == '$' || code == '.' || code == '|' || code == '!' || code == '>' || code == '<' ||
                       code == '^') {
                /* message-only controls */
            } else {
                bracket(&s);
            }
            continue;
        }
        const char *st = s;
        utf8_next(&s);
        while (st < s && rn < (int)sizeof run - 1)
            run[rn++] = *st++;
    }
    FLUSH();
    rpg_font_size = saved;
    return x;
}
