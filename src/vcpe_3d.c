/* VCPE 3D: depth buffer, camera matrices and indexed meshes on the GU (see vcpe_3d.h). */
#include <pspkernel.h>
#include <pspgu.h>
#include <math.h>
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

/* view frustum of the current camera, for vcpe_3d_visible */
static float fr_pos[3], fr_right[3], fr_up[3], fr_fwd[3], fr_th, fr_tv, fr_sh, fr_sv, fr_near, fr_far;

int vcpe_3d_visible(const float c[3], float r)
{
    float d[3] = {c[0] - fr_pos[0], c[1] - fr_pos[1], c[2] - fr_pos[2]};
    float z = d[0] * fr_fwd[0] + d[1] * fr_fwd[1] + d[2] * fr_fwd[2];
    if (z + r < fr_near || z - r > fr_far)
        return 0;
    float x = d[0] * fr_right[0] + d[1] * fr_right[1] + d[2] * fr_right[2];
    float y = d[0] * fr_up[0] + d[1] * fr_up[1] + d[2] * fr_up[2];
    if (fabsf(x) - r * fr_sh > z * fr_th) return 0;
    if (fabsf(y) - r * fr_sv > z * fr_tv) return 0;
    return 1;
}

int vcpe_3d_project(const float p[3], float *sx, float *sy)
{
    float d[3] = {p[0] - fr_pos[0], p[1] - fr_pos[1], p[2] - fr_pos[2]};
    float z = d[0] * fr_fwd[0] + d[1] * fr_fwd[1] + d[2] * fr_fwd[2];
    if (z < fr_near)
        return 0;
    float x = d[0] * fr_right[0] + d[1] * fr_right[1] + d[2] * fr_right[2];
    float y = d[0] * fr_up[0] + d[1] * fr_up[1] + d[2] * fr_up[2];
    *sx = VCPE_SCR_W * 0.5f * (1 + x / (z * fr_th));
    *sy = VCPE_SCR_H * 0.5f * (1 - y / (z * fr_tv));
    return 1;
}

void vcpe_3d_begin(const float pos[3], const float rot_deg[3], float fov_deg, float znear, float zfar)
{
    vcpe_gfx_flush();
    {
        const float k = GU_PI / 180.0f;
        float cx = cosf(rot_deg[0] * k), sx = sinf(rot_deg[0] * k), cy = cosf(rot_deg[1] * k), sy = sinf(rot_deg[1] * k);
        float cz = cosf(rot_deg[2] * k), sz = sinf(rot_deg[2] * k);
        /* columns of Ry * Rx * Rz: right, up, back */
        fr_right[0] = cy * cz + sy * sx * sz; fr_right[1] = cx * sz; fr_right[2] = -sy * cz + cy * sx * sz;
        fr_up[0] = -cy * sz + sy * sx * cz; fr_up[1] = cx * cz; fr_up[2] = sy * sz + cy * sx * cz;
        fr_fwd[0] = -sy * cx; fr_fwd[1] = sx; fr_fwd[2] = -cy * cx;
        for (int i = 0; i < 3; i++) fr_pos[i] = pos[i];
        fr_tv = tanf(fov_deg * 0.5f * k);
        fr_th = fr_tv * (float)VCPE_SCR_W / VCPE_SCR_H;
        fr_sv = sqrtf(1 + fr_tv * fr_tv);
        fr_sh = sqrtf(1 + fr_th * fr_th);
        fr_near = znear;
        fr_far = zfar;
    }
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
    sceGuTexFilter(GU_NEAREST_MIPMAP_NEAREST, GU_NEAREST);   /* mipmapped pages: far surfaces read smaller levels */
    sceGuTexLevelMode(GU_TEXTURE_AUTO, 0.0f);
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
    vcpe_gfx_flush();
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
    vcpe_gfx_flush();
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
    vcpe_gfx_flush();
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
    vcpe_gfx_flush();
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_FOG);
    sceGuEnable(GU_BLEND);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    vcpe_unbind();
}

void vcpe_3d_add(float k)
{
    vcpe_gfx_flush();
    unsigned a = (unsigned)((k < 0 ? 0 : k > 1 ? 1 : k) * 255.0f);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, a * 0x010101u, 0xFFFFFF);   /* dst + k x src */
    sceGuDepthMask(GU_TRUE);
}
