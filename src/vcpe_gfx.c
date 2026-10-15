/* VCPE graphics core: GU setup, frames, texture bundles and pages, cached render state, primitives. */
#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <malloc.h>
#include <string.h>
#include "vcpe_sys.h"
#include "vcpe_pak.h"
#include "vcpe_gfx.h"

#define BUF_W 512
#define FB_SIZE (BUF_W * VCPE_SCR_H * 4)
#ifndef MAX_BUNDLES
#define MAX_BUNDLES 64   /* games with more texture bundles pass -DMAX_BUNDLES=n (page tables hold up to 255) */
#endif
#ifndef VCPE_DLIST_KB
#define VCPE_DLIST_KB 512   /* display list per frame; -DVCPE_DLIST_KB=n trims it (PSP-1000) or grows it */
#endif
#define DLIST_BYTES (VCPE_DLIST_KB * 1024)
#define DLIST_RESERVE (16 * 1024)   /* kept free for state commands (4 bytes each) after the last vertex block */

static unsigned int __attribute__((aligned(64))) dlist[DLIST_BYTES / 4];
static const VcpePage *pages;
static const VcpeBundle *bundles;
static int nbundles, npages;
static uint8_t *bundle_mem[MAX_BUNDLES];
static uint8_t bundle_attached[MAX_BUNDLES];   /* game memory: not ours to free */
static uint8_t bundle_reload[MAX_BUNDLES];     /* placed in memory the system may take back while asleep */
static int reload_gen, reload_tries;
static uint8_t *slab;                          /* vcpe_bundle_slab: one block shared by a range of bundles */
static uint32_t slab_size;
static int slab_first = -1, slab_last = -2;
static int vsync = 1;
static int pipeline, pending;     /* pipeline: a finished list is queued on the GE, not shown yet */
static int bench_frames;          /* vcpe_gfx_bench: frames still drawn synchronously */
static int batch_on, batch_kind, batch_n;   /* batch_kind 1: textured sprites (VcpeVtx), 2: colour sprites (VcpeCVtx) */
#define BATCH_MAX 1024            /* vertices (two per rectangle) */
static VcpeVtx batch_t[BATCH_MAX];
static VcpeCVtx batch_c[BATCH_MAX];
static int bound_page = -2, bound_mode = -1, bound_white = -1, bound_filter = -1, opaque;
static const void *bound_tex;     /* vcpe_tex_claim: the module texture bound now (0: none / unknown) */
static int lazy_flush, upload_gen, flushed_gen = -1;
static uint32_t draw_offset;   /* VRAM offset of the buffer being drawn this frame */
static unsigned last_vcount;

uint32_t vcpe_gfx_draw_offset(void) { return draw_offset; }
void vcpe_gfx_state_reset(void)
{
    vcpe_gfx_flush();
    bound_page = -2; bound_mode = -1; bound_white = -1; bound_filter = -1;
    bound_tex = 0;
}
uint32_t vcpe_gfx_frame;   /* frames begun (caches that must not change memory the current frame uses) */
int vcpe_stat_binds, vcpe_stat_quads, vcpe_stat_draws, vcpe_stat_fps, vcpe_stat_build_us, vcpe_stat_gpu_wait_us;
int vcpe_stat_ge_us, vcpe_stat_vblanks = 1, vcpe_stat_vblank_misses, vcpe_stat_dlist_kb, vcpe_stat_dlist_peak_kb;
int vcpe_stat_dlist_full, vcpe_stat_reload_fail;
int vcpe_bind_epoch;   /* bumped whenever this module changes the bound texture / CLUT (other binders compare it) */
float vcpe_stat_area;

int vcpe_gfx_init(void)
{
    void *draw_fb = (void *)0, *disp_fb = (void *)FB_SIZE;
    sceGuInit();
    sceGuStart(GU_DIRECT, dlist);
    sceGuDrawBuffer(GU_PSM_8888, draw_fb, BUF_W);
    sceGuDispBuffer(VCPE_SCR_W, VCPE_SCR_H, disp_fb, BUF_W);
    sceGuOffset(2048 - (VCPE_SCR_W / 2), 2048 - (VCPE_SCR_H / 2));
    sceGuViewport(2048, 2048, VCPE_SCR_W, VCPE_SCR_H);
    sceGuScissor(0, 0, VCPE_SCR_W, VCPE_SCR_H);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_CULL_FACE);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuShadeModel(GU_SMOOTH);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
    last_vcount = sceDisplayGetVcount();
    return 0;
}

void vcpe_gfx_shutdown(void)
{
    vcpe_gfx_pipeline(0);
    sceGuTerm();
    vcpe_pak_close();
}

void vcpe_gfx_tables(const VcpePage *p, const VcpeBundle *b, int nb)
{
    pages = p;
    bundles = b;
    nbundles = nb > MAX_BUNDLES ? MAX_BUNDLES : nb;
    /* the bundles list every page: the table's length bounds vcpe_bind_page / vcpe_page_size */
    npages = 0;
    for (int i = 0; i < nbundles; i++)
        if (b[i].first_page + b[i].npages > npages)
            npages = b[i].first_page + b[i].npages;
}

/* display-list memory for this frame; 0 when the list is full (the draw is dropped and counted, instead of writing
 * past the end of the list into whatever the linker put after it) */
void *vcpe_gfx_mem(int size)
{
    if (sceGuCheckList() + size + 8 > DLIST_BYTES - DLIST_RESERVE) {
        vcpe_stat_dlist_full++;
        return 0;
    }
    return sceGuGetMemory(size);
}

static SceInt64 frame_t0, fps_t0;
static int fps_frames;
int vcpe_stat_frame_us;   /* last frame: vcpe_gfx_begin until the GPU finished (vblank wait not included) */

/* ------------------------------------------------------------------ batches */
void vcpe_gfx_flush(void)
{
    if (!batch_n)
        return;
    if (batch_kind == 1) {
        VcpeVtx *v = vcpe_gfx_mem(batch_n * sizeof(VcpeVtx));
        if (v) {
            memcpy(v, batch_t, batch_n * sizeof(VcpeVtx));
            sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, batch_n, 0, v);
        }
    } else {
        VcpeCVtx *v = vcpe_gfx_mem(batch_n * sizeof(VcpeCVtx));
        if (v) {
            memcpy(v, batch_c, batch_n * sizeof(VcpeCVtx));
            sceGuDisable(GU_TEXTURE_2D);
            sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, batch_n, 0, v);
            sceGuEnable(GU_TEXTURE_2D);
        }
    }
    vcpe_stat_draws++;
    batch_n = 0;
}

void vcpe_gfx_batch(int on)
{
    vcpe_gfx_flush();
    batch_on = on;
}

/* ------------------------------------------------------------------ frames */
static void fps_tick(void)
{
    SceInt64 t = sceKernelGetSystemTimeWide();
    if (++fps_frames >= 30 || t - fps_t0 > 1000000) {
        vcpe_stat_fps = (int)((fps_frames * 1000000LL + (t - fps_t0) / 2) / (t - fps_t0 + 1));
        fps_frames = 0;
        fps_t0 = t;
    }
}

/* a frame was shown: count the vblanks since the last one (more than one = a missed vblank) */
static void shown(void)
{
    unsigned vc = sceDisplayGetVcount();
    int d = (int)(vc - last_vcount);
    last_vcount = vc;
    vcpe_stat_vblanks = d > 0 ? d : 1;
    if (vsync && d > 1)
        vcpe_stat_vblank_misses += d - 1;
    fps_tick();
}

float vcpe_dt(void)
{
    extern int vcpe_autotest __attribute__((weak));
    if (&vcpe_autotest && vcpe_autotest)
        return 1.0f / 59.94f;   /* recordings: every frame is one vblank */
    int n = vcpe_stat_vblanks < 1 ? 1 : vcpe_stat_vblanks > 6 ? 6 : vcpe_stat_vblanks;   /* clamp after a stall */
    return n * (1.0f / 59.94f);
}

static void ge_done(SceInt64 t)
{
    vcpe_stat_gpu_wait_us = (int)(sceKernelGetSystemTimeWide() - t);
    vcpe_stat_frame_us = vcpe_stat_build_us + vcpe_stat_gpu_wait_us;
}

void vcpe_gfx_gpu_wait(void)
{
    if (pending == 1) {
        SceInt64 t = sceKernelGetSystemTimeWide();
        sceGuSync(0, 0);
        ge_done(t);
        pending = 2;   /* drawn, not shown */
    }
}

void vcpe_gfx_present(void)
{
    if (!pending)
        return;
    vcpe_gfx_gpu_wait();
    if (vsync)
        sceDisplayWaitVblankStart();
    draw_offset = (uint32_t)(uintptr_t)sceGuSwapBuffers();
    pending = 0;
    shown();
}

int vcpe_gfx_pending(void) { return pending != 0; }

void vcpe_gfx_pipeline(int on)
{
    if (!on)
        vcpe_gfx_present();
    pipeline = on;
    vcpe_present_hook = on ? vcpe_gfx_present : 0;   /* autotest reads and fatal screens show the last frame first */
}

void vcpe_gfx_bench(int frames) { bench_frames = frames > 0 ? frames : 0; }

static int in_frame;
int vcpe_gfx_in_frame(void) { return in_frame; }

void vcpe_gfx_begin(uint32_t clear)
{
    vcpe_gfx_present();
    if (reload_gen != vcpe_resume_gen)
        vcpe_bundles_reload();
    in_frame = 1;
    frame_t0 = sceKernelGetSystemTimeWide();
    vcpe_gfx_frame++;
    vcpe_stat_draws = 0;
    sceGuStart(GU_DIRECT, dlist);
    if (opaque) {   /* a frame never starts with blending off */
        sceGuEnable(GU_BLEND);
        opaque = 0;
    }
    sceGuClearColor(clear);
    sceGuClear(GU_COLOR_BUFFER_BIT);
    vcpe_bind_epoch++;
    bound_page = -2;
    bound_mode = -1;
    bound_white = -1;
    bound_filter = -1;
    bound_tex = 0;
}

void vcpe_gfx_end(void)
{
    in_frame = 0;
    vcpe_gfx_flush();
    sceGuFinish();
    SceInt64 t = sceKernelGetSystemTimeWide();
    vcpe_stat_build_us = (int)(t - frame_t0);
    vcpe_stat_dlist_kb = (sceGuCheckList() + 1023) / 1024;
    if (vcpe_stat_dlist_kb > vcpe_stat_dlist_peak_kb)
        vcpe_stat_dlist_peak_kb = vcpe_stat_dlist_kb;
    if (pipeline && !bench_frames) {
        /* the GE draws this frame while the game updates the next one; vcpe_gfx_begin shows it */
        pending = 1;
        return;
    }
    sceGuSync(0, 0);
    ge_done(t);
    vcpe_stat_ge_us = vcpe_stat_gpu_wait_us;   /* drawn with nothing overlapping: the GE's own time */
    if (bench_frames)
        bench_frames--;
    if (vsync)
        sceDisplayWaitVblankStart();
    draw_offset = (uint32_t)(uintptr_t)sceGuSwapBuffers();   /* the next frame draws into the other one */
    shown();
}

void vcpe_gfx_vsync(int on) { vsync = on; }

/* ------------------------------------------------------------------ bundles */
int vcpe_bundle_loaded(int b) { return b >= 0 && b < nbundles && bundle_mem[b] != 0; }
uint8_t *vcpe_bundle_mem(int b) { return b >= 0 && b < nbundles ? bundle_mem[b] : 0; }

static int in_slab(int b) { return slab && b >= slab_first && b <= slab_last; }

int vcpe_bundle_load(int b)
{
    if (b < 0 || b >= nbundles)
        return -1;
    if (bundle_mem[b])
        return 0;
    if (in_slab(b)) {
        for (int o = slab_first; o <= slab_last; o++)
            if (o != b)
                vcpe_bundle_unload(o);
        return vcpe_bundle_load_at(b, slab, slab_size, 0);
    }
    int st = vcpe_step_begin("Loading textures %d: %d pages, %u KB", b, bundles[b].npages,
                             (unsigned)(bundles[b].size / 1024));
    bundle_mem[b] = vcpe_pak_read_alloc(bundles[b].off, bundles[b].size);
    vcpe_step_end(st, bundle_mem[b] != 0);
    vcpe_tex_uploaded();
    return bundle_mem[b] ? 0 : -1;
}

int vcpe_bundle_load_at(int b, void *mem, uint32_t cap, int reload)
{
    if (b < 0 || b >= nbundles || !mem || bundles[b].size > cap)
        return -1;
    vcpe_bundle_unload(b);
    int st = vcpe_step_begin("Loading textures %d to %s: %u KB", b,
                             mem == slab ? "block" : reload ? "volatile memory" : "game memory",
                             (unsigned)(bundles[b].size / 1024));
    if (vcpe_pak_read(bundles[b].off, mem, bundles[b].size) < 0) {
        vcpe_step_end(st, 0);
        return -1;
    }
    vcpe_step_end(st, 1);
    sceKernelDcacheWritebackRange(mem, bundles[b].size);
    vcpe_tex_uploaded();
    bundle_mem[b] = mem;
    bundle_attached[b] = 1;
    bundle_reload[b] = reload != 0;
    return 0;
}

int vcpe_bundle_slab(int first, int last)
{
    uint32_t need = 0;
    if (first < 0 || last >= nbundles || first > last)
        return -1;
    for (int b = first; b <= last; b++)
        if (bundles[b].size > need)
            need = bundles[b].size;
    for (int b = 0; b < nbundles; b++)
        if (bundle_mem[b] && bundle_mem[b] == slab)
            vcpe_bundle_unload(b);
    if (slab && need <= slab_size) {
        /* the block already fits the new range: keep it (freeing and allocating again could fragment the heap) */
        slab_first = first;
        slab_last = last;
        return 0;
    }
    free(slab);
    int st = vcpe_step_begin("Reserving texture block %d-%d: %u KB", first, last, (unsigned)(need / 1024));
    slab = memalign(64, need ? need : 64);
    vcpe_step_end(st, slab != 0);
    slab_size = slab ? need : 0;
    slab_first = slab ? first : -1;
    slab_last = slab ? last : -2;
    return slab ? 0 : -1;
}

/* weak: games that link vcpe_load show its screen while the bundles are read again */
void vcpe_loading_begin(uint32_t total_bytes) __attribute__((weak));
void vcpe_loading_end(void) __attribute__((weak));
int vcpe_loading_active(void) __attribute__((weak));

int vcpe_resume_pending(void)
{
    if (reload_gen == vcpe_resume_gen)
        return 0;
    for (int b = 0; b < nbundles; b++)
        if (bundle_mem[b] && bundle_reload[b])
            return 1;
    reload_gen = vcpe_resume_gen;   /* nothing to read again */
    return 0;
}

/* the system may have used the volatile memory while the console slept: read those bundles again, behind the
 * loading screen when the game links it. A read that fails is tried again at the next frame (3 times) */
int vcpe_bundles_reload(void)
{
    if (!vcpe_resume_pending())
        return 0;
    if (in_frame) {   /* never inside a frame: the GE may be reading the memory */
        vcpe_gfx_flush();
        sceGuSync(0, 0);
    }
    vcpe_gfx_gpu_wait();
    uint32_t total = 0;
    for (int b = 0; b < nbundles; b++)
        if (bundle_mem[b] && bundle_reload[b])
            total += bundles[b].size;
    int gen = vcpe_resume_gen, failed = 0;
    reload_gen = gen;   /* first, so a loading screen drawn in between does not start another reload */
    int screen = vcpe_loading_begin && !in_frame && !vcpe_loading_active();
    if (screen)
        vcpe_loading_begin(total);
    for (int b = 0; b < nbundles; b++)
        if (bundle_mem[b] && bundle_reload[b]) {
            int st = vcpe_step_begin("Reloading textures %d after sleep", b);
            int ok = vcpe_pak_read(bundles[b].off, bundle_mem[b], bundles[b].size) == 0;
            vcpe_step_end(st, ok);
            sceKernelDcacheWritebackRange(bundle_mem[b], bundles[b].size);
            if (!ok) {
                failed++;
                vcpe_stat_reload_fail++;
            }
        }
    sceKernelDcacheWritebackAll();
    vcpe_tex_uploaded();
    if (screen)
        vcpe_loading_end();
    if (failed && ++reload_tries < 3)
        reload_gen = gen - 1;   /* try again next frame */
    else
        reload_tries = 0;
    return failed ? -1 : 1;
}

void vcpe_bundle_attach(int b, uint8_t *mem)
{
    if (b < 0 || b >= nbundles)
        return;
    vcpe_bundle_unload(b);
    bundle_mem[b] = mem;
    bundle_attached[b] = mem != 0;
    vcpe_tex_uploaded();
}

void vcpe_bundle_unload(int b)
{
    if (b < 0 || b >= nbundles || !bundle_mem[b])
        return;
    int st = vcpe_step_begin("Unloading textures %d: %u KB", b, (unsigned)(bundles[b].size / 1024));
    vcpe_gfx_flush();
    vcpe_gfx_gpu_wait();
    sceGuSync(0, 0);   /* inside a frame the GE may still be drawing this list (GU_DIRECT) */
    if (!bundle_attached[b])
        free(bundle_mem[b]);
    bundle_attached[b] = 0;
    bundle_reload[b] = 0;
    bundle_mem[b] = 0;
    bound_page = -2;
    vcpe_step_end(st, 1);
}

void vcpe_bundles_keep(const uint8_t *list, int n)
{
    /* free first so the new pages fit */
    for (int b = 0; b < nbundles; b++) {
        int need = 0;
        for (int i = 0; i < n; i++)
            if (list[i] == b)
                need = 1;
        if (!need)
            vcpe_bundle_unload(b);
    }
    for (int i = 0; i < n; i++)
        vcpe_bundle_load(list[i]);
}

/* ------------------------------------------------------------------ state */
void vcpe_page_size(int p, int *w, int *h)
{
    if (p < 0 || p >= npages) {
        *w = *h = 1;
        return;
    }
    *w = pages[p].w;
    *h = pages[p].h;
}

void vcpe_tex_uploaded(void) { upload_gen++; }
void vcpe_gfx_lazy_texflush(int on) { lazy_flush = on; flushed_gen = -1; }

void vcpe_tex_flush(void)
{
    if (lazy_flush && flushed_gen == upload_gen)
        return;
    sceGuTexFlush();
    flushed_gen = upload_gen;
}

int vcpe_tex_bound(const void *key) { return key && key == bound_tex && bound_page == -2; }

void vcpe_tex_claim(const void *key)
{
    vcpe_gfx_flush();
    vcpe_bind_epoch++;   /* other modules' cached texture/palette are no longer bound */
    bound_page = -2;
    bound_tex = key;
}

int vcpe_bind_page(int p)
{
    if (p == bound_page)
        return 1;
    if (p < 0 || p >= npages)
        return 0;
    vcpe_gfx_flush();
    vcpe_stat_binds++;
    vcpe_bind_epoch++;
    bound_tex = 0;
    const VcpePage *pg = &pages[p];
    uint8_t *base = bundle_mem[pg->bundle];
    if (!base)
        return 0;
    if (pg->psm == 4) {
        sceGuClutMode(GU_PSM_8888, 0, 0x0f, 0);
        sceGuClutLoad(2, base + pg->clut);
        sceGuTexMode(GU_PSM_T4, 0, 0, pg->swz);
    } else if (pg->psm == 8) {
        sceGuClutMode(GU_PSM_8888, 0, 0xff, 0);
        sceGuClutLoad(32, base + pg->clut);
        sceGuTexMode(GU_PSM_T8, 0, 0, pg->swz);
    } else if (pg->psm == 16 || pg->psm == 32) {
        sceGuTexMode(pg->psm == 16 ? GU_PSM_4444 : GU_PSM_8888, 0, 0, pg->swz);
    } else {
        return 0;
    }
    if (pg->nmips) {
        /* mipmapped (3D): the levels follow the full-size image; the GE picks one by distance */
        int bpp2 = pg->psm == 4 ? 1 : pg->psm == 8 ? 2 : pg->psm == 16 ? 4 : 8;
        uint32_t off = pg->data;
        int mode = pg->psm == 4 ? GU_PSM_T4 : pg->psm == 8 ? GU_PSM_T8 : pg->psm == 16 ? GU_PSM_4444 : GU_PSM_8888;
        sceGuTexMode(mode, pg->nmips, 0, pg->swz);
        for (int l = 0; l <= pg->nmips; l++) {
            int w = pg->w >> l, h = pg->h >> l;
            sceGuTexImage(l, w, h, w, base + off);
            off += ((uint32_t)w * h * bpp2 / 2 + 15) & ~15u;
        }
    } else {
        sceGuTexImage(0, pg->w, pg->h, pg->w, base + pg->data);
    }
    vcpe_tex_flush();
    bound_page = p;
    return 1;
}

void vcpe_unbind(void) { bound_page = -2; bound_tex = 0; }   /* the game bound a texture itself (flush before) */

void vcpe_blend(int additive)
{
    if (additive == bound_mode)
        return;
    vcpe_gfx_flush();
    if (additive == 2)
        sceGuBlendFunc(GU_REVERSE_SUBTRACT, GU_SRC_ALPHA, GU_FIX, 0, 0xFFFFFFFF);
    else if (additive)
        sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_FIX, 0, 0xFFFFFFFF);
    else
        sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    bound_mode = additive;
}

void vcpe_opaque(int on)
{
    on = on != 0;
    if (on == opaque)
        return;
    vcpe_gfx_flush();
    if (on)
        sceGuDisable(GU_BLEND);
    else
        sceGuEnable(GU_BLEND);
    opaque = on;
}

void vcpe_tex_white(int on)
{
    if (on == bound_white)
        return;
    vcpe_gfx_flush();
    sceGuTexFunc(on ? GU_TFX_ADD : GU_TFX_MODULATE, GU_TCC_RGBA);
    bound_white = on;
}

void vcpe_filter(int nearest)
{
    if (nearest == bound_filter)
        return;
    vcpe_gfx_flush();
    sceGuTexFilter(nearest ? GU_NEAREST : GU_LINEAR, nearest ? GU_NEAREST : GU_LINEAR);
    bound_filter = nearest;
}

/* ------------------------------------------------------------------ primitives */
void vcpe_quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t color)
{
    if (x1 < x0) { float t = x0; x0 = x1; x1 = t; t = u0; u0 = u1; u1 = t; }
    if (y1 < y0) { float t = y0; y0 = y1; y1 = t; t = v0; v0 = v1; v1 = t; }
    if (x1 < 0 || y1 < 0 || x0 > VCPE_SCR_W || y0 > VCPE_SCR_H)
        return;
    vcpe_stat_quads++;
    vcpe_stat_area += (x1 - x0) * (y1 - y0);
    VcpeVtx *v;
    if (batch_on) {
        if (batch_kind != 1 || batch_n + 2 > BATCH_MAX) {
            vcpe_gfx_flush();
            batch_kind = 1;
        }
        v = batch_t + batch_n;
        batch_n += 2;
    } else if (!(v = vcpe_gfx_mem(2 * sizeof(VcpeVtx)))) {
        return;
    }
    v[0].u = u0; v[0].v = v0; v[0].color = color; v[0].x = x0; v[0].y = y0; v[0].z = 0;
    v[1].u = u1; v[1].v = v1; v[1].color = color; v[1].x = x1; v[1].y = y1; v[1].z = 0;
    if (!batch_on) {
        vcpe_stat_draws++;
        sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, 0, v);
    }
}

void vcpe_fill(float x, float y, float w, float h, uint32_t color)
{
    if ((color >> 24) == 0)
        return;
    vcpe_stat_quads++;                 /* fills count as quads too in the perf lines */
    vcpe_stat_area += w * h;
    vcpe_blend(0);
    if (batch_on) {
        if (batch_kind != 2 || batch_n + 2 > BATCH_MAX) {
            vcpe_gfx_flush();
            batch_kind = 2;
        }
        VcpeCVtx *v = batch_c + batch_n;
        v[0].color = v[1].color = color;
        v[0].x = x; v[0].y = y; v[0].z = 0;
        v[1].x = x + w; v[1].y = y + h; v[1].z = 0;
        batch_n += 2;
        return;
    }
    VcpeCVtx *v = vcpe_gfx_mem(2 * sizeof(VcpeCVtx));
    if (!v)
        return;
    sceGuDisable(GU_TEXTURE_2D);
    v[0].color = v[1].color = color;
    v[0].x = x; v[0].y = y; v[0].z = 0;
    v[1].x = x + w; v[1].y = y + h; v[1].z = 0;
    vcpe_stat_draws++;
    sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, 0, v);
    sceGuEnable(GU_TEXTURE_2D);
}

void vcpe_strip(const float *xy, const uint32_t *colors, int n, int additive)
{
    if (n < 3)
        return;
    vcpe_gfx_flush();
    vcpe_blend(additive);
    VcpeCVtx *v = vcpe_gfx_mem(n * sizeof(VcpeCVtx));
    if (!v)
        return;
    sceGuDisable(GU_TEXTURE_2D);
    for (int i = 0; i < n; i++) {
        v[i].color = colors[i];
        v[i].x = xy[i * 2];
        v[i].y = xy[i * 2 + 1];
        v[i].z = 0;
    }
    sceGuDrawArray(GU_TRIANGLE_STRIP, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, n, 0, v);
    sceGuEnable(GU_TEXTURE_2D);
}

void vcpe_clip(int x, int y, int w, int h)
{
    vcpe_gfx_flush();
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    sceGuScissor(x, y, x + w, y + h);
}

void vcpe_clip_reset(void) { vcpe_gfx_flush(); sceGuScissor(0, 0, VCPE_SCR_W, VCPE_SCR_H); }
