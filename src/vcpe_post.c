/* VCPE post effects (see vcpe_post.h). */

/* HOW THIS FILE WORKS (post effects without pixel shaders)
 *
 * The GE has no pixel shaders, so colour curves (posterize, gamma, contrast) can't be computed per pixel. The trick
 * used here: the frame is drawn into an off-screen buffer, then copied to the real frame buffer three times, once per
 * colour channel, treating the 32-bit image as an 8-bit indexed texture:
 * - the GE's palette mode can pick which byte of each 32-bit texel is the index (shift 0, 8 or 16 = red, green,
 *   blue), and mask it to 8 bits;
 * - clut[ch] maps that byte to a colour containing only that channel, run through the curve;
 * - pass 1 writes red, passes 2 and 3 are added on top (blend: dst + src), giving the curved image. So any
 *   per-channel curve costs three full-screen copies, done in 64-pixel strips because the texture cache handles
 *   narrow strips much better.
 *
 * Grain / dither: a 32x32 noise tile (random, or a 4x4 ordered Bayer pattern) is added before the curves, so
 * posterized colour bands break up into grain instead of hard edges. */
#include <pspkernel.h>
#include <pspgu.h>
#include <pspge.h>
#include <string.h>
#include "vcpe_gfx.h"
#include "vcpe_post.h"

#define BUF_W 512
#define FB_SIZE (BUF_W * VCPE_SCR_H * 4)
/* The off-screen buffer's VRAM offset: after both frame buffers and the 16-bit depth buffer. */
#define OFF_OFFSET (2 * FB_SIZE + BUF_W * VCPE_SCR_H * 2)   /* after the two frame buffers and the 16-bit depth buffer */

static uint32_t __attribute__((aligned(64))) clut[3][256];   /* colours with only their own channel set */
static int active, grain, grain_still;
static uint32_t __attribute__((aligned(64))) noise[32 * 32];   /* grey noise tile, alpha unused */
static unsigned seed = 12345;

/* Ordered dither: the Bayer matrix spreads 16 thresholds evenly over every 4x4 block; it does not move between
 * frames. */
void vcpe_post_dither(int amount)
{
    static const int bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    grain = amount < 0 ? 0 : amount > 255 ? 255 : amount;
    grain_still = 1;
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++) {
            uint32_t v = (uint32_t)(bayer[y & 3][x & 3] * 255 + 7) / 15;
            noise[y * 32 + x] = 0xFF000000u | v | v << 8 | v << 16;
        }
    sceKernelDcacheWritebackRange(noise, sizeof noise);
}

/* Film grain: random grey noise (a simple LCG), shifted every frame so it moves. */
void vcpe_post_grain(int amount)
{
    grain = amount < 0 ? 0 : amount > 255 ? 255 : amount;
    grain_still = 0;
    for (int i = 0; i < 32 * 32; i++) {
        seed = seed * 1103515245u + 12345u;
        uint32_t v = (seed >> 16) & 0xFF;
        noise[i] = 0xFF000000u | v | v << 8 | v << 16;
    }
    sceKernelDcacheWritebackRange(noise, sizeof noise);
}

/* Build the three per-channel palettes from curves (each maps 0..255 to 0..255). */
void vcpe_post_curves(const uint8_t r[256], const uint8_t g[256], const uint8_t b[256])
{
    for (int i = 0; i < 256; i++) {
        clut[0][i] = 0xFF000000u | r[i];
        clut[1][i] = 0xFF000000u | ((uint32_t)g[i] << 8);
        clut[2][i] = 0xFF000000u | ((uint32_t)b[i] << 16);
    }
    sceKernelDcacheWritebackRange(clut, sizeof clut);
}

/* Posterize: round every channel down to `levels` steps. */
void vcpe_post_posterize(int levels)
{
    uint8_t c[256];
    for (int i = 0; i < 256; i++) {
        int q = i * levels / 255;                 /* floor(v * levels) */
        int v = q * 255 / levels;
        c[i] = (uint8_t)(v > 255 ? 255 : v);
    }
    vcpe_post_curves(c, c, c);
}

/* Redirect drawing into the off-screen buffer until vcpe_post_end. */
void vcpe_post_begin(void)
{
    vcpe_gfx_flush();
    sceGuDrawBufferList(GU_PSM_8888, (void *)OFF_OFFSET, BUF_W);
    sceGuClearColor(0xFF000000);
    sceGuClear(GU_COLOR_BUFFER_BIT);
    active = 1;
}

/* Apply grain, then the three channel passes into the real frame buffer, then restore the 2D state. */
void vcpe_post_end(void)
{
    if (!active)
        return;
    vcpe_gfx_flush();
    active = 0;
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_FOG);
    sceGuEnable(GU_TEXTURE_2D);
    if (grain) {
        /* grain: the noise tile (repeated, at a new offset every frame) times amount, added to the picture */
        sceGuTexMode(GU_PSM_8888, 0, 0, 0);
        sceGuTexImage(0, 32, 32, 32, noise);
        sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
        sceGuTexFilter(GU_NEAREST, GU_NEAREST);
        sceGuTexWrap(GU_REPEAT, GU_REPEAT);
        sceGuTexFlush();
        sceGuEnable(GU_BLEND);
        uint32_t a = (uint32_t)grain * 0x010101u;
        sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, a, 0xFFFFFFFF);
        float ou = 0, ov = 0;
        if (!grain_still) {
            seed = seed * 1103515245u + 12345u;
            ou = (float)((seed >> 8) & 31);
            ov = (float)((seed >> 16) & 31);
        }
        VcpeVtx *v = vcpe_gfx_mem(2 * sizeof(VcpeVtx));
        if (v) {
        v[0].u = ou; v[0].v = ov; v[0].color = 0xFFFFFFFF; v[0].x = 0; v[0].y = 0; v[0].z = 0;
        v[1].u = ou + VCPE_SCR_W; v[1].v = ov + VCPE_SCR_H; v[1].color = 0xFFFFFFFF; v[1].x = VCPE_SCR_W; v[1].y = VCPE_SCR_H; v[1].z = 0;
        sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, 0, v);
        }
    }
    /* Draw into the real frame buffer again, reading the off-screen buffer as a 32-bit indexed (T32) texture. */
    sceGuDrawBufferList(GU_PSM_8888, (void *)vcpe_gfx_draw_offset(), BUF_W);
    sceGuTexMode(GU_PSM_T32, 0, 0, 0);
    sceGuTexImage(0, BUF_W, 512, BUF_W, (uint8_t *)sceGeEdramGetAddr() + OFF_OFFSET);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexFlush();
    sceGuTexSync();
    for (int ch = 0; ch < 3; ch++) {
        if (ch == 0) {
            sceGuDisable(GU_BLEND);
        } else {
            sceGuEnable(GU_BLEND);
            sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0xFFFFFFFF, 0xFFFFFFFF);   /* add this channel */
        }
        /* Index = (texel >> ch*8) & 0xFF: the red, green or blue byte. */
        sceGuClutMode(GU_PSM_8888, ch * 8, 0xFF, 0);
        sceGuClutLoad(256 / 8, clut[ch]);
        /* 64-pixel wide strips: the texture cache works on small spans */
        for (int x = 0; x < VCPE_SCR_W; x += 64) {
            VcpeVtx *v = vcpe_gfx_mem(2 * sizeof(VcpeVtx));
            if (!v)
                break;
            int x1 = x + 64 > VCPE_SCR_W ? VCPE_SCR_W : x + 64;
            v[0].u = (float)x; v[0].v = 0; v[0].color = 0xFFFFFFFF; v[0].x = (float)x; v[0].y = 0; v[0].z = 0;
            v[1].u = (float)x1; v[1].v = VCPE_SCR_H; v[1].color = 0xFFFFFFFF; v[1].x = (float)x1; v[1].y = VCPE_SCR_H; v[1].z = 0;
            sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, 0, v);
        }
    }
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    vcpe_gfx_state_reset();
}
