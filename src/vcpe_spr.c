/* VCPE sprites: per-image palettes on cached 256x256 pages, GameMaker-style draws and bitmap-font text. */
#include <pspgu.h>
#include <pspkernel.h>
#include <malloc.h>
#include <math.h>
#include <string.h>
#include "vcpe_pak.h"
#include "vcpe_gfx.h"
#include "vcpe_spr.h"

#define PAGE 256

static VcpeSprHeader hdr;
static VcpeSprImage *images;
static VcpeSprFrame *frames;
static VcpeSprPiece *pieces;
static VcpeSprPage *page_tab;
static VcpeSprFont *fonts;
static VcpeSprGlyph *glyphs;
static uint8_t *pal_blob;
static uint8_t **page_mem;
static uint32_t *page_used;         /* frame clock of the last draw */
static int32_t *lru_prev, *lru_next; /* resident pages, most recently drawn first (4.0: O(1) eviction) */
static int lru_head = -1, lru_tail = -1;
static uint32_t base_off, budget, resident;
static uint32_t clock_ = 1;
static int all_loaded;            /* vcpe_spr_load_all: every page is resident, nothing is evicted */
static uint8_t *all_mem;          /* its single block, when it got one */
static uint8_t *arena, *arena_own;  /* vcpe_spr_arena: fixed slots instead of malloc per page */
static uint32_t slot_bytes, nslots;
static int32_t *slot_free;        /* stack of free slot indices */
static int nfree;
static float view_x, view_y;
static int additive, flash;
static int bound_page = -1, bound_epoch = -1;
static const void *bound_clut;
static const void *clut_override;
int vcpe_spr_loads, vcpe_spr_fails;
static int16_t (*ascii)[128];   /* per font: glyph index of each ASCII code (-1 none), built once */
/* sin of degrees from a 1024-step table (rotated sprites: libm trig is slow on the PSP's FPU), built at init */
static float sin_tab[1025];

static void *read_table(uint32_t off, uint32_t size)
{
    if (!size)
        return 0;
    void *p = memalign(64, size);
    if (p && vcpe_pak_read(base_off + off, p, size)) {
        free(p);
        return 0;
    }
    return p;
}

static void free_tables(void)
{
    free(images); free(frames); free(pieces); free(page_tab); free(fonts); free(glyphs); free(pal_blob);
    free(page_mem); free(page_used); free(ascii); free(lru_prev); free(lru_next);
    images = 0; frames = 0; pieces = 0; page_tab = 0; fonts = 0; glyphs = 0; pal_blob = 0;
    page_mem = 0; page_used = 0; ascii = 0; lru_prev = lru_next = 0;
    hdr.nimages = hdr.nfonts = hdr.npages = 0;
}

int vcpe_spr_init(uint32_t section_off, uint32_t budget_bytes)
{
    base_off = section_off;
    budget = budget_bytes;
    if (sin_tab[256] == 0)
        for (int i = 0; i <= 1024; i++)
            sin_tab[i] = sinf(i * (2 * (float)M_PI / 1024));
    if (vcpe_pak_read(section_off, &hdr, sizeof hdr) || memcmp(hdr.magic, "VSPR", 4)) {
        hdr.nimages = hdr.nfonts = hdr.npages = 0;
        return -1;
    }
    images = read_table(hdr.images, hdr.nimages * sizeof *images);
    frames = read_table(hdr.frames, hdr.nframes * sizeof *frames);
    pieces = read_table(hdr.pieces, hdr.npieces * sizeof *pieces);
    page_tab = read_table(hdr.pages, hdr.npages * sizeof *page_tab);
    fonts = read_table(hdr.fonts, hdr.nfonts * sizeof *fonts);
    glyphs = read_table(hdr.glyphs, hdr.nglyphs * sizeof *glyphs);
    pal_blob = read_table(hdr.pal, hdr.pal_size);
    page_mem = calloc(hdr.npages, sizeof *page_mem);
    if (hdr.nfonts && fonts && glyphs && (ascii = malloc(hdr.nfonts * sizeof *ascii)) != 0)
        for (uint32_t f = 0; f < hdr.nfonts; f++) {
            for (int c = 0; c < 128; c++)
                ascii[f][c] = -1;
            for (int k = 0; k < fonts[f].nglyphs; k++)
                if (glyphs[fonts[f].glyph0 + k].ch < 128 && ascii[f][glyphs[fonts[f].glyph0 + k].ch] < 0)
                    ascii[f][glyphs[fonts[f].glyph0 + k].ch] = (int16_t)k;
        }
    page_used = calloc(hdr.npages, sizeof *page_used);
    lru_prev = malloc(hdr.npages * sizeof *lru_prev + 4);
    lru_next = malloc(hdr.npages * sizeof *lru_next + 4);
    if (!images || !frames || !pieces || !page_tab || !pal_blob || !page_mem || !page_used || !lru_prev || !lru_next) {
        free_tables();   /* nothing half-made stays behind: every draw is then a no-op */
        return -2;
    }
    lru_head = lru_tail = -1;
    sceKernelDcacheWritebackAll();
    return 0;
}

int vcpe_spr_count(void) { return (int)hdr.nimages; }

const uint32_t *vcpe_spr_clut(int img, int *count)
{
    const VcpeSprImage *im = vcpe_spr_image(img);
    if (!im)
        return 0;
    if (count)
        *count = im->bpp == 4 ? 16 : 256;
    return (const uint32_t *)(pal_blob + im->pal);
}

void vcpe_spr_clut_override(const uint32_t *clut) { clut_override = clut; }
const VcpeSprImage *vcpe_spr_image(int img) { return img >= 0 && img < (int)hdr.nimages ? &images[img] : 0; }
int vcpe_spr_resident_kb(void) { return (int)(resident >> 10); }
void vcpe_spr_view(float x, float y) { view_x = x; view_y = y; }
void vcpe_spr_additive(int on) { additive = on; }
void vcpe_spr_flash(int on) { flash = on; }

void vcpe_spr_frame_begin(void)
{
    clock_++;
}

static uint32_t page_bytes(int p) { return page_tab[p].bpp == 4 ? PAGE * PAGE / 2 : PAGE * PAGE; }

/* ------------------------------------------------------------------ LRU list of resident pages */
static void lru_unlink(int p)
{
    if (lru_prev[p] >= 0) lru_next[lru_prev[p]] = lru_next[p]; else lru_head = lru_next[p];
    if (lru_next[p] >= 0) lru_prev[lru_next[p]] = lru_prev[p]; else lru_tail = lru_prev[p];
}

static void lru_front(int p)
{
    lru_prev[p] = -1;
    lru_next[p] = lru_head;
    if (lru_head >= 0) lru_prev[lru_head] = p; else lru_tail = p;
    lru_head = p;
}

static void page_drop(int p)
{
    lru_unlink(p);
    if (arena)
        slot_free[nfree++] = (int32_t)((page_mem[p] - arena) / slot_bytes);
    else if (!all_mem)
        free(page_mem[p]);
    page_mem[p] = 0;
    resident -= page_bytes(p);
    if (bound_page == p)
        bound_page = -1;
}

/* make room: drop the least recently drawn pages, never one drawn this frame (the GE may read it) */
static void evict_for(uint32_t need)
{
    if (all_loaded)
        return;
    int waited = 0;
    while (arena ? nfree == 0 : resident + need > budget) {
        int p = lru_tail;
        if (p < 0 || page_used[p] >= clock_)
            return;   /* everything is in use this frame: go over budget rather than drop a page the GE reads */
        if (!waited) {
            vcpe_gfx_flush();
            vcpe_gfx_gpu_wait();   /* pipeline: the frame handed over may still read this page */
            waited = 1;
        }
        page_drop(p);
    }
}

static uint8_t *page_read(int p, uint32_t n)
{
    uint32_t off = base_off + hdr.page_data + page_tab[p].off;
    if (!arena)
        return vcpe_pak_read_alloc(off, n);
    if (!nfree)
        return 0;
    uint8_t *mem = arena + (uint32_t)slot_free[nfree - 1] * slot_bytes;
    if (vcpe_pak_read(off, mem, n))
        return 0;
    nfree--;
    sceKernelDcacheWritebackRange(mem, n);
    return mem;
}

static uint8_t *page_get(int p)
{
    if (!page_mem[p]) {
        uint32_t n = page_bytes(p);
        evict_for(n);
        page_mem[p] = page_read(p, n);
        if (!page_mem[p]) {
            /* out of memory: drop everything not drawn this frame and try once more */
            uint32_t keep = budget;
            budget = 0;
            evict_for(n);
            budget = keep;
            page_mem[p] = page_read(p, n);
            if (!page_mem[p]) {
                vcpe_spr_fails++;
                return 0;
            }
        }
        resident += n;
        vcpe_spr_loads++;
        vcpe_tex_uploaded();
        lru_front(p);
    } else if (page_used[p] != clock_ && lru_head != p) {
        lru_unlink(p);
        lru_front(p);
    }
    page_used[p] = clock_;
    return page_mem[p];
}

int vcpe_spr_arena(void *mem, uint32_t size)
{
    if (all_loaded || !page_tab)
        return -1;
    uint32_t slot = PAGE * PAGE / 2;
    for (uint32_t p = 0; p < hdr.npages; p++)
        if (page_bytes((int)p) > slot)
            slot = page_bytes((int)p);
    vcpe_gfx_flush();
    vcpe_gfx_gpu_wait();
    while (lru_head >= 0)
        page_drop(lru_head);
    free(arena_own);
    free(slot_free);
    arena = arena_own = 0;
    slot_free = 0;
    nfree = 0;
    if (!size)
        return 0;   /* back to malloc per page */
    uint32_t n = size / slot;
    if (!n || !(slot_free = malloc(n * sizeof *slot_free)))
        return -1;
    if (!mem && !(mem = arena_own = memalign(64, n * slot))) {
        free(slot_free);
        slot_free = 0;
        return -1;
    }
    arena = mem;
    slot_bytes = slot;
    nslots = n;
    for (uint32_t i = 0; i < n; i++)
        slot_free[nfree++] = (int32_t)(n - 1 - i);   /* slot 0 is used first */
    return (int)n;
}

int vcpe_spr_load_all(void (*progress)(int done, int total))
{
    if (all_loaded)
        return 0;
    uint32_t total = 0;
    for (uint32_t p = 0; p < hdr.npages; p++) {
        uint32_t end = page_tab[p].off + page_bytes((int)p);
        if (end > total)
            total = end;
    }
    if (arena)
        vcpe_spr_arena(0, 0);   /* the pages get memory of their own below */
    uint8_t *mem = memalign(64, total);
    if (!mem) {
        /* 4.0: no single block that large (a fragmented heap): one allocation per page instead */
        int done = 0;
        for (uint32_t p = 0; p < hdr.npages; p++) {
            if (!page_mem[p]) {
                uint32_t n = page_bytes((int)p);
                page_mem[p] = vcpe_pak_read_alloc(base_off + hdr.page_data + page_tab[p].off, n);
                if (!page_mem[p]) {
                    vcpe_spr_fails++;
                    return -1;   /* the pages read so far stay as a warm cache; budget eviction still applies */
                }
                resident += n;
                vcpe_spr_loads++;
                lru_front((int)p);
                page_used[p] = 0;
            }
            done += (int)page_bytes((int)p);
            if (progress)
                progress(done, (int)total);
        }
        vcpe_tex_uploaded();
        all_loaded = 1;
        return 0;
    }
    const uint32_t chunk = 512 * 1024;
    for (uint32_t o = 0; o < total; o += chunk) {
        uint32_t n = total - o < chunk ? total - o : chunk;
        if (vcpe_pak_read(base_off + hdr.page_data + o, mem + o, n)) {
            free(mem);
            return -2;
        }
        if (progress)
            progress((int)(o + n), (int)total);
    }
    sceKernelDcacheWritebackRange(mem, total);
    vcpe_gfx_flush();
    vcpe_gfx_gpu_wait();
    /* drop pages loaded one by one before, then point every page into the block */
    for (uint32_t p = 0; p < hdr.npages; p++) {
        if (page_mem[p])
            free(page_mem[p]);
        page_mem[p] = mem + page_tab[p].off;
    }
    lru_head = lru_tail = -1;
    all_mem = mem;
    all_loaded = 1;
    resident = total;
    bound_page = -1;
    vcpe_tex_uploaded();
    return 0;
}

void vcpe_spr_preload(int img)
{
    const VcpeSprImage *im = vcpe_spr_image(img);
    if (!im)
        return;
    for (int f = 0; f < im->nframes; f++) {
        const VcpeSprFrame *fr = &frames[im->first_frame + f];
        for (int i = 0; i < fr->npieces; i++)
            page_get(pieces[fr->first_piece + i].page);
    }
}

/* bind the page and the image palette; 0 when the page can't be loaded */
static int bind(const VcpeSprImage *im, int p)
{
    uint8_t *mem = page_get(p);
    if (!mem)
        return 0;
    if (bound_epoch != vcpe_bind_epoch) {
        bound_page = -1;
        bound_clut = 0;
    }
    const void *clut = clut_override ? clut_override : pal_blob + im->pal;
    if (clut != bound_clut || p != bound_page)
        vcpe_gfx_flush();   /* the batch was drawn with the old palette / page */
    if (clut != bound_clut) {
        if (im->bpp == 4) {
            sceGuClutMode(GU_PSM_8888, 0, 0x0f, 0);
            sceGuClutLoad(2, clut);
        } else {
            sceGuClutMode(GU_PSM_8888, 0, 0xff, 0);
            sceGuClutLoad(32, clut);
        }
        bound_clut = clut;
    }
    if (p != bound_page) {
        sceGuTexMode(page_tab[p].bpp == 4 ? GU_PSM_T4 : GU_PSM_T8, 0, 0, 1);
        sceGuTexImage(0, PAGE, PAGE, PAGE, mem);
        vcpe_tex_flush();
        bound_page = p;
        vcpe_stat_binds++;
    }
    vcpe_unbind();   /* the engine's own page cache no longer matches the GE */
    bound_epoch = vcpe_bind_epoch;
    return 1;
}

static const VcpeSprFrame *get_frame(const VcpeSprImage *im, int frame)
{
    if (!im || !im->nframes)
        return 0;
    frame %= im->nframes;
    if (frame < 0)
        frame += im->nframes;
    return &frames[im->first_frame + frame];
}

static void prep(void)
{
    vcpe_blend(additive);
    vcpe_tex_white(flash);
    vcpe_filter(1);
}

/* one textured quad given 4 screen corners (TL, TR, BL, BR) */
static void quad4(const float *xy, float u0, float v0, float u1, float v1, uint32_t color)
{
    vcpe_gfx_flush();
    VcpeVtx *v = vcpe_gfx_mem(4 * sizeof(VcpeVtx));
    if (!v)
        return;
    float us[4] = { u0, u1, u0, u1 }, vs[4] = { v0, v0, v1, v1 };
    for (int i = 0; i < 4; i++) {
        v[i].u = us[i];
        v[i].v = vs[i];
        v[i].color = color;
        v[i].x = xy[i * 2];
        v[i].y = xy[i * 2 + 1];
        v[i].z = 0;
    }
    vcpe_stat_quads++;
    sceGuDrawArray(GU_TRIANGLE_STRIP, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 4, 0, v);
}

/* draw the part [l, l+w) x [t, t+h) of a frame (frame pixels); the frame point (px, py) lands on screen (sx, sy) */
static float tab_sin(float deg)
{
    float t = deg * (1024 / 360.0f);
    int i = (int)t;
    if (t < i)
        i--;
    float f = t - i;
    i &= 1023;
    return sin_tab[i] + (sin_tab[i + 1] - sin_tab[i]) * f;
}

static void draw_rect(const VcpeSprImage *im, const VcpeSprFrame *fr, int l, int t, int w, int h, float px, float py,
                      float sx, float sy, float xs, float ys, float angle, uint32_t color)
{
    float c = 1, s = 0;
    int rot = angle != 0;
    if (rot) {
        c = tab_sin(angle + 90);
        s = tab_sin(angle);
    }
    prep();
    for (int i = 0; i < fr->npieces; i++) {
        const VcpeSprPiece *pc = &pieces[fr->first_piece + i];
        int x0 = pc->dx > l ? pc->dx : l, y0 = pc->dy > t ? pc->dy : t;
        int x1 = pc->dx + pc->w < l + w ? pc->dx + pc->w : l + w;
        int y1 = pc->dy + pc->h < t + h ? pc->dy + pc->h : t + h;
        if (x1 <= x0 || y1 <= y0)
            continue;
        float u0 = pc->u + (x0 - pc->dx), v0 = pc->v + (y0 - pc->dy);
        float u1 = u0 + (x1 - x0), v1 = v0 + (y1 - y0);
        float lx0 = (x0 - px) * xs, ly0 = (y0 - py) * ys, lx1 = (x1 - px) * xs, ly1 = (y1 - py) * ys;
        if (!rot) {
            float qx0 = sx + lx0, qx1 = sx + lx1, qy0 = sy + ly0, qy1 = sy + ly1;
            float mnx = qx0 < qx1 ? qx0 : qx1, mxx = qx0 < qx1 ? qx1 : qx0;
            float mny = qy0 < qy1 ? qy0 : qy1, mxy = qy0 < qy1 ? qy1 : qy0;
            if (mxx < 0 || mxy < 0 || mnx > VCPE_SCR_W || mny > VCPE_SCR_H)
                continue;
            if (!bind(im, pc->page))
                continue;
            vcpe_quad(qx0, qy0, qx1, qy1, u0, v0, u1, v1, color);
        } else {
            float xy[8];
            float lx[4] = { lx0, lx1, lx0, lx1 }, ly[4] = { ly0, ly0, ly1, ly1 };
            int vis = 0;
            for (int k = 0; k < 4; k++) {
                /* GameMaker: counter-clockwise on screen (y down) */
                xy[k * 2] = sx + lx[k] * c + ly[k] * s;
                xy[k * 2 + 1] = sy - lx[k] * s + ly[k] * c;
                if (xy[k * 2] > -64 && xy[k * 2] < VCPE_SCR_W + 64 && xy[k * 2 + 1] > -64 &&
                    xy[k * 2 + 1] < VCPE_SCR_H + 64)
                    vis = 1;
            }
            if (!vis && (fabsf(lx1 - lx0) < 400 && fabsf(ly1 - ly0) < 400))
                continue;
            if (!bind(im, pc->page))
                continue;
            quad4(xy, u0, v0, u1, v1, color);
        }
    }
}

void vcpe_spr_draw(int img, int frame, float x, float y, float xs, float ys, float angle, uint32_t color)
{
    const VcpeSprImage *im = vcpe_spr_image(img);
    const VcpeSprFrame *fr = get_frame(im, frame);
    if (!fr || (color >> 24) == 0)
        return;
    float sx = x - view_x, sy = y - view_y;
    /* the whole sprite off screen: nothing to do (a bound on any rotation) */
    float ax = fabsf(xs), ay = fabsf(ys);
    float reach = (im->w + im->h + fabsf((float)im->ox) + fabsf((float)im->oy)) * (ax > ay ? ax : ay);
    if (sx + reach < 0 || sy + reach < 0 || sx - reach > VCPE_SCR_W || sy - reach > VCPE_SCR_H)
        return;
    if (angle == 0) {
        sx = floorf(sx + 0.001f);
        sy = floorf(sy + 0.001f);
    }
    draw_rect(im, fr, 0, 0, im->w, im->h, im->ox, im->oy, sx, sy, xs, ys, angle, color);
}

void vcpe_spr_draw_part(int img, int frame, int left, int top, int w, int h, float x, float y, float xs, float ys,
                        uint32_t color)
{
    const VcpeSprImage *im = vcpe_spr_image(img);
    const VcpeSprFrame *fr = get_frame(im, frame);
    if (!fr || (color >> 24) == 0)
        return;
    float sx = floorf(x - view_x + 0.001f), sy = floorf(y - view_y + 0.001f);
    if (sx > VCPE_SCR_W || sy > VCPE_SCR_H || sx + w * fabsf(xs) < 0 || sy + h * fabsf(ys) < 0)
        return;
    draw_rect(im, fr, left, top, w, h, left, top, sx, sy, xs, ys, 0, color);
}

void vcpe_spr_draw_tiled(int img, int frame, float x, float y, int tile_x, int tile_y, float xs, float ys,
                         uint32_t color)
{
    const VcpeSprImage *im = vcpe_spr_image(img);
    if (!im || !im->w || !im->h)
        return;
    float w = im->w * xs, h = im->h * ys;
    float sx = floorf(x - view_x), sy = floorf(y - view_y);
    float x0 = sx, y0 = sy;
    int nx = 1, ny = 1;
    if (tile_x && w > 0) {
        x0 = sx - ceilf(sx / w) * w;
        nx = (int)((VCPE_SCR_W - x0) / w) + 1;
    }
    if (tile_y && h > 0) {
        y0 = sy - ceilf(sy / h) * h;
        ny = (int)((VCPE_SCR_H - y0) / h) + 1;
    }
    for (int j = 0; j < ny; j++)
        for (int i = 0; i < nx; i++)
            vcpe_spr_draw(img, frame, x0 + i * w + view_x + im->ox * xs, y0 + j * h + view_y + im->oy * ys, xs, ys,
                          0, color);
}

int vcpe_spr_frame_box(int img, int frame, int *x0, int *y0, int *x1, int *y1)
{
    const VcpeSprImage *im = vcpe_spr_image(img);
    const VcpeSprFrame *fr = get_frame(im, frame);
    if (!fr || !fr->npieces)
        return 0;
    int a = 1 << 30, b = 1 << 30, c = -(1 << 30), d = -(1 << 30);
    for (int i = 0; i < fr->npieces; i++) {
        const VcpeSprPiece *pc = &pieces[fr->first_piece + i];
        if (pc->dx < a) a = pc->dx;
        if (pc->dy < b) b = pc->dy;
        if (pc->dx + pc->w > c) c = pc->dx + pc->w;
        if (pc->dy + pc->h > d) d = pc->dy + pc->h;
    }
    *x0 = a; *y0 = b; *x1 = c; *y1 = d;
    return 1;
}

/* ------------------------------------------------------------------ text */
const VcpeSprFont *vcpe_spr_font(int f) { return f >= 0 && f < (int)hdr.nfonts ? &fonts[f] : 0; }

static const VcpeSprGlyph *glyph(const VcpeSprFont *f, unsigned ch)
{
    if (ch < 128 && ascii) {
        int k = ascii[f - fonts][ch];
        return k >= 0 ? glyphs + f->glyph0 + k : 0;
    }
    int lo = 0, hi = f->nglyphs - 1;
    const VcpeSprGlyph *g = glyphs + f->glyph0;
    while (lo <= hi) {
        int m = (lo + hi) >> 1;
        if (g[m].ch == ch)
            return &g[m];
        if (g[m].ch < ch)
            lo = m + 1;
        else
            hi = m - 1;
    }
    return 0;
}

static int line_break(const char *s, int *skip)
{
    if (s[0] == '\n') { *skip = 1; return 1; }
    if (s[0] == '\r') { *skip = s[1] == '\n' ? 2 : 1; return 1; }
    if (s[0] == '#') { *skip = 1; return 1; }
    return 0;
}

static float line_width(const VcpeSprFont *f, const char *s, const char **end)
{
    float w = 0;
    int skip;
    while (*s && !line_break(s, &skip)) {
        unsigned ch = (unsigned char)*s;
        if (ch == '\\' && s[1] == '#') {
            ch = '#';
            s++;
        }
        const VcpeSprGlyph *g = glyph(f, ch);
        if (g)
            w += g->shift;
        s++;
    }
    *end = s;
    return w;
}

float vcpe_spr_text_width(int font, const char *s, float scale)
{
    const VcpeSprFont *f = vcpe_spr_font(font);
    if (!f || !s)
        return 0;
    float best = 0;
    for (;;) {
        const char *e;
        float w = line_width(f, s, &e);
        if (w > best)
            best = w;
        int skip;
        if (!*e || !line_break(e, &skip))
            break;
        s = e + skip;
    }
    return best * scale;
}

float vcpe_spr_text_height(int font, const char *s, float scale)
{
    const VcpeSprFont *f = vcpe_spr_font(font);
    if (!f || !s)
        return 0;
    int n = 1, skip;
    for (; *s; s++)
        if (line_break(s, &skip)) {
            n++;
            s += skip - 1;
        }
    return n * f->height * scale;
}

void vcpe_spr_text(int font, float x, float y, const char *s, int halign, int valign, float scale, uint32_t color)
{
    const VcpeSprFont *f = vcpe_spr_font(font);
    if (!f || !s || (color >> 24) == 0)
        return;
    const VcpeSprImage *im = vcpe_spr_image(f->image);
    const VcpeSprFrame *fr = get_frame(im, 0);
    if (!fr)
        return;
    float h = vcpe_spr_text_height(font, s, scale);
    float cy = y - view_y - (valign == 1 ? floorf(h / 2) : valign == 2 ? h : 0);
    for (;;) {
        const char *e;
        float w = line_width(f, s, &e) * scale;
        float cx = x - view_x - (halign == 1 ? floorf(w / 2) : halign == 2 ? w : 0);
        cx = floorf(cx);
        float ly = floorf(cy);
        for (const char *p = s; p < e; p++) {
            unsigned ch = (unsigned char)*p;
            if (ch == '\\' && p[1] == '#') {
                ch = '#';
                p++;
            }
            const VcpeSprGlyph *g = glyph(f, ch);
            if (!g)
                continue;
            if (g->w && g->h)
                draw_rect(im, fr, g->x, g->y, g->w, g->h, g->x, g->y, cx + g->off * scale, ly, scale, scale, 0,
                          color);
            cx += g->shift * scale;
        }
        cy += f->height * scale;
        int skip;
        if (!*e || !line_break(e, &skip))
            break;
        s = e + skip;
    }
}

void vcpe_spr_draw_part_ex(int img,int frame,int left,int top,int w,int h,float x,float y,float xs,float ys,float angle,uint32_t color)
{
 const VcpeSprImage *im=vcpe_spr_image(img);
 const VcpeSprFrame *fr=get_frame(im,frame);
 if(!fr||!(color>>24))return;
 draw_rect(im,fr,left,top,w,h,left+w*.5f,top+h*.5f,x-view_x,y-view_y,xs,ys,angle,color);
}
