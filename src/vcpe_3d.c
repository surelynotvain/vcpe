/* VCPE 3D: depth buffer, camera matrices and indexed meshes on the GU (see vcpe_3d.h). */
#include <pspkernel.h>
#include <pspgu.h>
#include <pspgum.h>
#include "vcpe_gfx.h"
#include "vcpe_3d.h"

#define BUF_W 512
#define FB_SIZE (BUF_W * VCPE_SCR_H * 4)
#define ZB_ADDR ((void *)(2 * FB_SIZE))   /* after the draw and display buffers: 512 x 272 x 16 bit */

int vcpe_stat_tris;
static float fog_start, fog_end;
static uint32_t fog_color;

void vcpe_3d_fog(float start, float end, uint32_t color)
{
    fog_start = start;
    fog_end = end;
    fog_color = color;
}

void vcpe_3d_begin(const float pos[3], const float rot_deg[3], float fov_deg, float znear, float zfar)
{
    sceGuDepthBuffer(ZB_ADDR, BUF_W);
    sceGuDepthRange(65535, 0);
    sceGuDepthFunc(GU_GEQUAL);
    sceGuClearDepth(0);
    sceGuClear(GU_DEPTH_BUFFER_BIT);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuDepthMask(GU_FALSE);
    sceGuEnable(GU_CLIP_PLANES);
    sceGuDisable(GU_CULL_FACE);
    sceGuDisable(GU_BLEND);
    sceGuTexWrap(GU_REPEAT, GU_REPEAT);
    sceGuEnable(GU_ALPHA_TEST);                 /* cut-out textures */
    sceGuAlphaFunc(GU_GREATER, 0x20, 0xFF);
    vcpe_filter(1);
    vcpe_blend(0);
    vcpe_tex_white(0);
    if (fog_end > fog_start) {
        sceGuFog(fog_start, fog_end, fog_color);
        sceGuEnable(GU_FOG);
    }

    sceGumMatrixMode(GU_PROJECTION);
    sceGumLoadIdentity();
    sceGumPerspective(fov_deg, (float)VCPE_SCR_W / VCPE_SCR_H, znear, zfar);

    /* view = inverse(T * Ry * Rx * Rz): Godot's default YXZ euler order */
    const float d2r = GU_PI / 180.0f;
    sceGumMatrixMode(GU_VIEW);
    sceGumLoadIdentity();
    sceGumRotateZ(-rot_deg[2] * d2r);
    sceGumRotateX(-rot_deg[0] * d2r);
    sceGumRotateY(-rot_deg[1] * d2r);
    ScePspFVector3 t = {-pos[0], -pos[1], -pos[2]};
    sceGumTranslate(&t);

    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();
    vcpe_stat_tris = 0;
}

void vcpe_3d_model(const float *m)
{
    sceGumMatrixMode(GU_MODEL);
    if (!m) {
        sceGumLoadIdentity();
        return;
    }
    /* row-major (column vectors) -> the GU's layout: x, y, z, w are the columns */
    ScePspFMatrix4 g = {{m[0], m[4], m[8], m[12]}, {m[1], m[5], m[9], m[13]}, {m[2], m[6], m[10], m[14]}, {m[3], m[7], m[11], m[15]}};
    sceGumLoadMatrix(&g);
}

void vcpe_3d_draw(int page, const void *verts, const uint16_t *idx, int nidx)
{
    if (page >= 0) {
        if (!vcpe_bind_page(page))
            return;
        sceGuEnable(GU_TEXTURE_2D);
    } else {
        vcpe_unbind();
        sceGuDisable(GU_TEXTURE_2D);
    }
    sceGumDrawArray(GU_TRIANGLES, GU_INDEX_16BIT | GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D,
                    nidx, idx, verts);
    vcpe_stat_tris += nidx / 3;
    if (page < 0)
        sceGuEnable(GU_TEXTURE_2D);
}

void vcpe_3d_alpha(float a)
{
    if (a >= 0.999f) {
        sceGuDisable(GU_BLEND);
        sceGuDepthMask(GU_FALSE);
        return;
    }
    unsigned k = (unsigned)((a < 0 ? 0 : a) * 255.0f);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, k | (k << 8) | (k << 16), (255 - k) | ((255 - k) << 8) | ((255 - k) << 16));
    sceGuDepthMask(GU_TRUE);   /* see-through: no depth writes */
}

void vcpe_3d_end(void)
{
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_FOG);
    sceGuEnable(GU_BLEND);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    vcpe_unbind();
}
