/* VCPE post effects (see vcpe_post.h). */
#include <pspkernel.h>
#include <pspgu.h>
#include <pspge.h>
#include <string.h>
#include "vcpe_gfx.h"
#include "vcpe_post.h"

#define BUF_W 512
#define FB_SIZE (BUF_W * VCPE_SCR_H * 4)
#define OFF_OFFSET (2 * FB_SIZE + BUF_W * VCPE_SCR_H * 2)   /* after the two frame buffers and the 16-bit depth buffer */

static uint32_t __attribute__((aligned(64))) clut[3][256];   /* colours with only their own channel set */
static int active, grain;
static uint32_t __attribute__((aligned(64))) noise[32 * 32];   /* grey noise tile, alpha unused */
static unsigned seed = 12345;

void vcpe_post_grain(int amount)
{
    grain = amount < 0 ? 0 : amount > 255 ? 255 : amount;
    for (int i = 0; i < 32 * 32; i++) {
        seed = seed * 1103515245u + 12345u;
        uint32_t v = (seed >> 16) & 0xFF;
        noise[i] = 0xFF000000u | v | v << 8 | v << 16;
    }
    sceKernelDcacheWritebackRange(noise, sizeof noise);
}

void vcpe_post_curves(const uint8_t r[256], const uint8_t g[256], const uint8_t b[256])
{
    for (int i = 0; i < 256; i++) {
        clut[0][i] = 0xFF000000u | r[i];
        clut[1][i] = 0xFF000000u | ((uint32_t)g[i] << 8);
        clut[2][i] = 0xFF000000u | ((uint32_t)b[i] << 16);
    }
    sceKernelDcacheWritebackRange(clut, sizeof clut);
}

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

void vcpe_post_begin(void)
{
    sceGuDrawBufferList(GU_PSM_8888, (void *)OFF_OFFSET, BUF_W);
    sceGuClearColor(0xFF000000);
    sceGuClear(GU_COLOR_BUFFER_BIT);
    active = 1;
}

void vcpe_post_end(void)
{
    if (!active)
        return;
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
        seed = seed * 1103515245u + 12345u;
        float ou = (float)((seed >> 8) & 31), ov = (float)((seed >> 16) & 31);
        VcpeVtx *v = sceGuGetMemory(2 * sizeof(VcpeVtx));
        v[0].u = ou; v[0].v = ov; v[0].color = 0xFFFFFFFF; v[0].x = 0; v[0].y = 0; v[0].z = 0;
        v[1].u = ou + VCPE_SCR_W; v[1].v = ov + VCPE_SCR_H; v[1].color = 0xFFFFFFFF; v[1].x = VCPE_SCR_W; v[1].y = VCPE_SCR_H; v[1].z = 0;
        sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, 0, v);
    }
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
        sceGuClutMode(GU_PSM_8888, ch * 8, 0xFF, 0);
        sceGuClutLoad(256 / 8, clut[ch]);
        /* 64-pixel wide strips: the texture cache works on small spans */
        for (int x = 0; x < VCPE_SCR_W; x += 64) {
            VcpeVtx *v = sceGuGetMemory(2 * sizeof(VcpeVtx));
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
