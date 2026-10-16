/* VCPE TrueType text (see vcpe_ttf.h). */

/* HOW THIS FILE WORKS
 *
 * stb_truetype (a single-header public-domain library) reads the font file and turns glyph outlines into 8-bit
 * coverage bitmaps ("rasterizing"). That is slow, so every rasterized glyph is kept in a cache:
 *
 * - two 512x512 8-bit textures ("pages"). The texel value is the glyph's coverage; the palette maps value i to white
 *   with alpha i, so the vertex colour tints the text and the coverage becomes its transparency (anti-aliasing);
 * - glyphs are packed on a page in rows ("shelf packing": left to right, then a new row under the tallest glyph);
 * - a hash table (slots) maps (font, size, glyph, sub-pixel phase) to the glyph's place on a page.
 *
 * When the current page is full, the other page is wiped and reused, but only if the frame being built has not drawn
 * from it (the GE would read glyphs that are gone). Pages carry a generation number, so slots pointing at a wiped
 * page are recognised as stale.
 *
 * Sub-pixel phases: the pen position is kept in quarter pixels, and each glyph is rasterized at the quarter-pixel
 * offset it lands on (up to 4 versions per glyph), so spacing follows the font exactly instead of snapping every
 * letter to whole pixels. */
#include <pspkernel.h>
#include <pspgu.h>
#include <malloc.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "vcpe_gfx.h"
#include "vcpe_ttf.h"
#include "vcpe_observe.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
/* Compile stb_truetype's code into this file only, as static functions (no symbol clashes with games). */
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include <stb_truetype.h>
#pragma GCC diagnostic pop

#define PAGE_W 512
#define PAGE_H 512
#define NPAGES 2
#define PHASES 4            /* sub-pixel pen positions per pixel */
/* Glyph index of every codepoint below 0x300 (Latin, Greek...) is cached in the font, others are looked up each time.
 */
#define CMAP_FAST 0x300     /* codepoints below this keep their glyph index in a table */
/* Hash table sizes must be powers of two (indexes are masked with size - 1). */
#define GLYPH_SLOTS 4096    /* cached rasterized glyphs (open addressing, both pages) */
#define KERN_SLOTS 2048
#define MAX_FONTS 16

/* One opened font: stb's parsed info plus vertical metrics in font units (upem_scale converts units to pixels). */
struct VcpeTtf {
    stbtt_fontinfo info;
    int id, ascent, descent, gap;    /* font units */
    float upem_scale;                /* pixels per font unit at px = 1 */
    int16_t cmap[CMAP_FAST];         /* glyph index + 1, 0 = not looked up yet */
};

typedef struct {
    /* A cached glyph: where it is on which page, its size, and its offset from the pen position. */
    uint32_t key;                    /* 0 = empty */
    uint16_t gl;
    uint8_t page, gen;
    uint16_t u, v, w, h;
    int16_t xoff, yoff;
} Slot;

typedef struct {
    /* A glyph page: its texels, the shelf packer's cursor (x, y, current row height), its generation, the frame it
     * was last drawn in, and the rows written since the last cache writeback. */
    uint8_t *tex;
    int x, y, row;                   /* shelf packer */
    uint8_t gen;
    uint32_t used_frame;
    int dirty_lo, dirty_hi;          /* rows written since the last data-cache writeback */
} Page;

int vcpe_stat_ttf_raster;
static Page pages[NPAGES];
static uint32_t __attribute__((aligned(16))) clut[256];
static Slot slots[GLYPH_SLOTS];
static int nslots, cur_page, ready, nfonts;
static struct { uint32_t key; int16_t kern; } kerns[KERN_SLOTS];

void vcpe_ttf_stat_reset(void) { vcpe_stat_ttf_raster = 0; }

/* Pages are independent allocations: the second 256 KiB page is paid for only when the first fills. Failure
 * leaves any existing page intact, and retry never overwrites an allocation it already owns. */
static int page_init(int p)
{
    Page *pg = &pages[p];
    if (pg->tex) return 1;
    pg->tex = memalign(64, PAGE_W * PAGE_H);
    if (!pg->tex) { vcpe_observe_fail("font atlas",PAGE_W*PAGE_H); return 0; }
    memset(pg->tex, 0, PAGE_W * PAGE_H);
    sceKernelDcacheWritebackRange(pg->tex, PAGE_W * PAGE_H);
    pg->used_frame = UINT32_MAX;
    pg->dirty_lo = PAGE_H;
    return 1;
}

uint32_t vcpe_ttf_resident_bytes(void)
{
    uint32_t bytes = 0;
    for (int p = 0; p < NPAGES; p++) if (pages[p].tex) bytes += PAGE_W * PAGE_H;
    return bytes;
}

/* Allocate the first page and build the alpha palette at first use. */
static int init(void)
{
    if (ready)
        return 1;
    if (!page_init(0)) return 0;
    pages[1].used_frame = UINT32_MAX;
    for (int i = 0; i < 256; i++)
        clut[i] = 0x00FFFFFFu | ((uint32_t)i << 24);
    sceKernelDcacheWritebackRange(clut, sizeof clut);
    ready = 1;
    return 1;
}

/* Parse a font file the game keeps in memory (stb reads it in place, it is not copied). */
VcpeTtf *vcpe_ttf_open(const void *data, uint32_t size)
{
    if (!data || size < 12 || nfonts >= MAX_FONTS)
        return 0;
    VcpeTtf *f = calloc(1, sizeof *f);
    if (!f)
        return 0;
    if (!stbtt_InitFont(&f->info, data, stbtt_GetFontOffsetForIndex(data, 0))) {
        free(f);
        return 0;
    }
    f->id = nfonts++;
    stbtt_GetFontVMetrics(&f->info, &f->ascent, &f->descent, &f->gap);
    f->upem_scale = stbtt_ScaleForMappingEmToPixels(&f->info, 1.0f);
    return f;
}

/* Metrics rounded the way FreeType reports them, so layouts made on a PC with FreeType line up. */
float vcpe_ttf_ascent(const VcpeTtf *f, float px) { return ceilf(f->ascent * f->upem_scale * px - 1e-4f); }
float vcpe_ttf_descent(const VcpeTtf *f, float px) { return floorf(f->descent * f->upem_scale * px + 1e-4f); }
float vcpe_ttf_line(const VcpeTtf *f, float px)
{
    return ceilf((f->ascent - f->descent + f->gap) * f->upem_scale * px - 1e-4f);
}

/* Codepoint to glyph index (0 = the font has no such character). The cmap table stores index+1 so 0 means "not looked
 * up". */
static int glyph_of(VcpeTtf *f, uint32_t cp)
{
    if (cp < CMAP_FAST) {
        if (!f->cmap[cp])
            f->cmap[cp] = (int16_t)(stbtt_FindGlyphIndex(&f->info, (int)cp) + 1);
        return f->cmap[cp] - 1;
    }
    return stbtt_FindGlyphIndex(&f->info, (int)cp);
}

int vcpe_ttf_has(const VcpeTtf *f, uint32_t cp) { return glyph_of((VcpeTtf *)f, cp) != 0; }

/* Decode one UTF-8 character and advance the pointer (1 to 4 bytes; malformed bytes are passed through as-is). */
static uint32_t utf8_next(const char **ps)
{
    const unsigned char *s = (const unsigned char *)*ps;
    uint32_t c = *s++;
    if (c >= 0xF0 && s[0] && s[1] && s[2]) {
        c = ((c & 7) << 18) | ((s[0] & 63) << 12) | ((s[1] & 63) << 6) | (s[2] & 63);
        s += 3;
    } else if (c >= 0xE0 && s[0] && s[1]) {
        c = ((c & 15) << 12) | ((s[0] & 63) << 6) | (s[1] & 63);
        s += 2;
    } else if (c >= 0xC0 && s[0]) {
        c = ((c & 31) << 6) | (s[0] & 63);
        s += 1;
    }
    *ps = (const char *)s;
    return c;
}

/* the glyph for a codepoint, '?' when the font lacks it */
static int glyph_or_q(VcpeTtf *f, uint32_t cp)
{
    int g = glyph_of(f, cp);
    return g ? g : glyph_of(f, '?');
}

/* Kerning between two glyphs (e.g. "AV" closer together), cached in a small hash table because stb looks it up
 * slowly. */
static int kern(VcpeTtf *f, int g1, int g2)
{
    if (!f->info.gpos && !f->info.kern)
        return 0;
    if (g1 >= 16384 || g2 >= 16384)
        return stbtt_GetGlyphKernAdvance(&f->info, g1, g2);   /* outside the cache key's range */
    uint32_t key = ((uint32_t)f->id << 28) ^ ((uint32_t)g1 << 14) ^ (uint32_t)g2 ^ 0x80000000u;
    uint32_t h = (key * 2654435761u) >> 21;   /* 11 bits */
    if (kerns[h].key == key)
        return kerns[h].kern;
    int k = stbtt_GetGlyphKernAdvance(&f->info, g1, g2);
    kerns[h].key = key;
    kerns[h].kern = (int16_t)k;
    return k;
}

/* Width of a string: advances plus kerning, summed in font units and scaled once at the end. */
float vcpe_ttf_width(VcpeTtf *f, float px, const char *s)
{
    if (!f || !s)
        return 0;
    float sc = f->upem_scale * px;
    int units = 0, prev = -1;
    while (*s) {
        int g = glyph_or_q(f, utf8_next(&s));
        int adv, lsb;
        stbtt_GetGlyphHMetrics(&f->info, g, &adv, &lsb);
        if (prev >= 0)
            units += kern(f, prev, g);
        units += adv;
        prev = g;
    }
    return units * sc;
}

/* ------------------------------------------------------------------ glyph cache */
/* Pack (font, size in 1/8 px, glyph, phase) into one 32-bit key for the hash table. */
static uint32_t slot_key(const VcpeTtf *f, int px8, int g, int phase)
{
    /* font 4 bits, size 9 bits (px * 8, up to 64 px), glyph 16 bits, phase 2 bits; never 0 */
    return ((uint32_t)f->id << 27) ^ ((uint32_t)(px8 & 511) << 18) ^ ((uint32_t)g << 2) ^ (uint32_t)phase ^ 0x40000000u;
}

/* Open addressing: start at the key's hash, walk forward to the matching slot or the first empty one. */
static Slot *slot_find(uint32_t key, uint16_t g)
{
    uint32_t h = (key * 2654435761u) >> 20;   /* 12 bits */
    for (int i = 0; i < GLYPH_SLOTS; i++) {
        Slot *s = &slots[(h + i) & (GLYPH_SLOTS - 1)];
        if (!s->key || (s->key == key && s->gl == g))
            return s;
    }
    return 0;
}

/* Erase one entry while retaining all open-addressing probe chains. */
static void slot_erase(unsigned hole)
{
    const unsigned mask = GLYPH_SLOTS - 1;
    slots[hole].key = 0;
    for (unsigned j = (hole + 1) & mask; slots[j].key; j = (j + 1) & mask) {
        unsigned home = (slots[j].key * 2654435761u) >> 20;
        if (((j - home) & mask) >= ((j - hole) & mask)) {
            slots[hole] = slots[j];
            hole = j;
            slots[hole].key = 0;
        }
    }
}

/* Backshift deletion preserves every collision chain in place, including chains crossing the end of the table.
 * No duplicate 80 KiB survivor array is needed. Repeat an index if a deleted-page entry moves into its hole. */
static void slots_drop_page(int p)
{
    for (unsigned i = 0; i < GLYPH_SLOTS; i++)
        while (slots[i].key && slots[i].page == p) { slot_erase(i); nslots--; }
}

/* Wipe a page for reuse. Only the rows the packer reached hold glyphs, so only those are cleared and written back. */
static void page_clear(int p)
{
    Page *pg = &pages[p];
    vcpe_gfx_gpu_wait();   /* the GE may still read the frame handed over (pipelining) */
    int used = pg->y + pg->row;   /* rows the shelf packer reached: the rest is still clear */
    if (used > PAGE_H)
        used = PAGE_H;
    if (pg->tex && used > 0) {
        memset(pg->tex, 0, used * PAGE_W);
        sceKernelDcacheWritebackRange(pg->tex, used * PAGE_W);
        vcpe_tex_uploaded();
    }
    pg->x = pg->y = pg->row = 0;
    pg->gen++;
    pg->dirty_lo = PAGE_H;
    pg->dirty_hi = 0;
    slots_drop_page(p);
}

void vcpe_ttf_cache_reset(void)
{
    if (!ready)
        return;
    for (int p = 0; p < NPAGES; p++)
        page_clear(p);
    memset(slots, 0, sizeof slots);
    nslots = 0;
    cur_page = 0;
}

/* room for a w x h glyph (1 texel gap); -1 when both pages are full and the other one is in use this frame */
/* Find room for a w x h glyph on the current page (shelf packing). If it is full, switch to the other page, wiping
 * it, unless this frame already drew from it. */
static int alloc(int w, int h, int *u, int *v)
{
    for (int tries = 0; tries < 2; tries++) {
        Page *pg = &pages[cur_page];
        if (pg->x + w + 1 > PAGE_W) {
            pg->x = 0;
            pg->y += pg->row;
            pg->row = 0;
        }
        if (pg->y + h + 1 <= PAGE_H && w + 1 <= PAGE_W) {
            *u = pg->x;
            *v = pg->y;
            pg->x += w + 1;
            if (h + 1 > pg->row)
                pg->row = h + 1;
            return cur_page;
        }
        /* this page is full: recycle the other one unless this frame already draws from it */
        int other = (cur_page + 1) % NPAGES;
        if (pages[other].used_frame == vcpe_gfx_frame)
            return -1;
        if (!page_init(other)) return -1;
        page_clear(other);
        cur_page = other;
    }
    return -1;
}

/* Return the cached glyph, rasterizing it on a page first if it is missing or its page was wiped. */
static const Slot *glyph_get(VcpeTtf *f, float px, int g, int phase)
{
    int px8 = (int)(px * 8 + 0.5f);
    uint32_t key = slot_key(f, px8, g, phase);
    Slot *s = slot_find(key, (uint16_t)g);
    if (s && s->key && s->gen == pages[s->page].gen)
        return s;
    /* A hash table more than 3/4 full gets slow: free a page this frame does not use (or both when possible). */
    if (!s || nslots > GLYPH_SLOTS * 3 / 4) {
        /* table crowded: drop a page this frame does not draw from (both when neither is in use) */
        int other = (cur_page + 1) % NPAGES;
        if (pages[cur_page].used_frame != vcpe_gfx_frame && pages[other].used_frame != vcpe_gfx_frame) {
            vcpe_ttf_cache_reset();
        } else if (pages[other].used_frame != vcpe_gfx_frame) {
            page_clear(other);
            cur_page = other;
        }
        s = slot_find(key, (uint16_t)g);
        if (!s)
            return 0;
    }
    float sc = f->upem_scale * px8 / 8.0f, sx = phase / (float)PHASES;
    int x0, y0, x1, y1;
    /* Glyph size at this scale and sub-pixel shift, then rasterize straight into the page (stride = page width). */
    stbtt_GetGlyphBitmapBoxSubpixel(&f->info, g, sc, sc, sx, 0, &x0, &y0, &x1, &y1);
    int w = x1 - x0, h = y1 - y0, u = 0, v = 0, p = 0;
    if (w > 0 && h > 0) {
        p = alloc(w, h, &u, &v);
        if (p < 0)
            return 0;
        Page *pg = &pages[p];
        stbtt_MakeGlyphBitmapSubpixel(&f->info, pg->tex + v * PAGE_W + u, w, h, PAGE_W, sc, sc, sx, 0, g);
        if (v < pg->dirty_lo) pg->dirty_lo = v;
        if (v + h > pg->dirty_hi) pg->dirty_hi = v + h;
        vcpe_stat_ttf_raster++;
    }
    /* alloc() may have recycled a page and moved hash entries; never reuse a pointer obtained before it. */
    s = slot_find(key, (uint16_t)g);
    if (!s) return 0;
    if (!s->key)
        nslots++;
    s->key = key;
    s->gl = (uint16_t)g;
    s->page = (uint8_t)p;
    s->gen = pages[p].gen;
    s->u = (uint16_t)u;
    s->v = (uint16_t)v;
    s->w = (uint16_t)(w > 0 ? w : 0);
    s->h = (uint16_t)(h > 0 ? h : 0);
    s->xoff = (int16_t)x0;
    s->yoff = (int16_t)y0;
    return s;
}

/* Bind a glyph page: write back new glyph rows from the CPU cache to RAM first (the GE reads RAM), and skip the
 * texture commands when the page is still bound and unchanged. */
static void bind(int p)
{
    Page *pg = &pages[p];
    int dirty = pg->dirty_hi > pg->dirty_lo;
    if (dirty) {
        sceKernelDcacheWritebackRange(pg->tex + pg->dirty_lo * PAGE_W, (pg->dirty_hi - pg->dirty_lo) * PAGE_W);
        pg->dirty_lo = PAGE_H;
        pg->dirty_hi = 0;
        vcpe_tex_uploaded();
    }
    vcpe_tex_white(0);
    if (!dirty && vcpe_tex_bound(pg))
        return;
    vcpe_tex_claim(pg);
    sceGuClutMode(GU_PSM_8888, 0, 0xff, 0);
    sceGuClutLoad(32, clut);
    sceGuTexMode(GU_PSM_T8, 0, 0, 0);
    sceGuTexImage(0, PAGE_W, PAGE_H, PAGE_W, pg->tex);
    vcpe_tex_flush();   /* new glyphs may sit where the GE cached blank texels earlier this frame */
}

#define MAX_LINE 256

/* Draw one line of text, pen starting at (x, y) on the baseline. */
float vcpe_ttf_draw(VcpeTtf *f, float px, float x, float y, const char *s, uint32_t color)
{
    if (!f || !s || !*s || px <= 0 || !init())
        return 0;
    if (px > 63.0f)
        px = 63.0f;   /* slot key limit */
    /* pass 1: lay the line out and make sure every glyph is on a page (nothing is drawn yet, so a page the GE
     * caches for this line already holds all of its glyphs) */
    /* Copies of the slots, not pointers: rasterizing a later glyph may recycle a page and rebuild the slot table. */
    static Slot gs[MAX_LINE];   /* copies: a page recycled later in this line rebuilds the slot table */
    static int16_t gx[MAX_LINE];
    float sc = f->upem_scale * px, pen = x;
    int by = (int)floorf(y + 0.5f), n = 0, prev = -1;
    const char *p = s;
    while (*p && n < MAX_LINE) {
        int g = glyph_or_q(f, utf8_next(&p));
        int adv, lsb;
        stbtt_GetGlyphHMetrics(&f->info, g, &adv, &lsb);
        if (prev >= 0)
            pen += kern(f, prev, g) * sc;
        float fx = floorf(pen);
        /* Split the pen into a whole pixel and a quarter-pixel phase; rounding up to a full pixel moves to the next
         * pixel. */
        int phase = (int)((pen - fx) * PHASES + 0.5f), ix = (int)fx;
        if (phase >= PHASES) {
            phase = 0;
            ix++;
        }
        const Slot *sl = glyph_get(f, px, g, phase);
        if (sl && sl->w) {
            pages[sl->page].used_frame = vcpe_gfx_frame;
            gs[n] = *sl;
            gx[n++] = (int16_t)(ix + sl->xoff);
        }
        pen += adv * sc;
        prev = g;
    }
    /* pass 2: quads, one bind per page change */
    /* Pass 2: emit the quads, binding a page only when the next glyph is on the other one. */
    vcpe_blend(0);
    vcpe_filter(1);
    int bound = -1;
    for (int i = 0; i < n; i++) {
        const Slot *sl = &gs[i];
        if (sl->page != bound) {
            bind(sl->page);
            bound = sl->page;
        }
        float qx = gx[i], qy = by + sl->yoff;
        vcpe_quad(qx, qy, qx + sl->w, qy + sl->h, sl->u, sl->v, sl->u + sl->w, sl->v + sl->h, color);
    }
    if (bound >= 0)
        vcpe_gfx_flush();   /* drawn now; the page stays bound for the next line */
    return pen - x;
}

/* rasterize a charset at every sub-pixel phase now (behind a loading screen), so a dialog that first shows these
 * characters does not rasterize them mid-frame. Returns the glyphs rasterized */
/* Rasterize characters at all 4 phases ahead of time (behind a loading screen), so the first dialog line does not
 * stall. */
int vcpe_ttf_prewarm(VcpeTtf *f, float px, const char *chars)
{
    if (!f || !chars || px <= 0 || !init())
        return 0;
    if (px > 63.0f)
        px = 63.0f;
    int before = vcpe_stat_ttf_raster;
    const char *p = chars;
    while (*p) {
        int g = glyph_or_q(f, utf8_next(&p));
        for (int ph = 0; ph < PHASES; ph++)
            glyph_get(f, px, g, ph);
    }
    return vcpe_stat_ttf_raster - before;
}
