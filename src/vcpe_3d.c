/* VCPE 3D: depth buffer, camera matrices and indexed meshes on the GU (see vcpe_3d.h). */

/* HOW THIS FILE WORKS
 *
 * The GE does 3D in hardware: it multiplies every vertex by three matrices (model: object to world; view: world to
 * camera; projection: camera to screen with perspective), clips triangles at the screen edges and keeps a depth
 * buffer so nearer surfaces hide farther ones. The engine's job is only to set those up:
 *
 * - vcpe_3d_begin: the camera (position, rotation, field of view) becomes the view and projection matrices, the depth
 *   buffer is cleared and enabled, and 3D-friendly state is set (repeat-wrapped textures, mipmaps, alpha test for
 *   cut-out textures, optional fog);
 * - vcpe_3d_model / vcpe_3d_draw: per-object matrix, then indexed triangle lists;
 * - vcpe_3d_end: back to the 2D state, so HUDs can be drawn on top.
 *
 * The depth buffer is 16-bit, in VRAM right after the two frame buffers; 2D-only games never touch it. Rotations
 * follow Godot's convention (Y, then X, then Z, in degrees), so scenes converted from Godot keep their numbers. */
#include <pspkernel.h>
#include <pspgu.h>
#include <math.h>
#include <pspgum.h>
#include <string.h>
#include "vcpe_gfx.h"
#define VCPE_CULL_WEAK
#include "vcpe_3d.h"
#undef VCPE_CULL_WEAK

#define BUF_W 512
#define FB_SIZE (BUF_W * VCPE_SCR_H * 4)
#define ZB_ADDR ((void *)(2 * FB_SIZE))   /* after the draw and display buffers: 512 x 272 x 16 bit */

/* Triangles drawn this frame (perf lines). */
int vcpe_stat_tris;
int vcpe_stat_cull_tests, vcpe_stat_culled, vcpe_stat_cull_tris, vcpe_stat_cull_nodes;
static int autocull = 1, active_3d;
static const float identity[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
static float model_matrix[16];
static VcpeCullPlanes camera_planes;

void vcpe_3d_autocull(int on) { autocull=on!=0; }
int vcpe_3d_autocull_enabled(void) { return autocull; }
int vcpe_3d_prepare(const void *v,uint32_t nv,const uint16_t *idx,uint32_t ni)
{ return vcpe_cull_prepare ? vcpe_cull_prepare(v,nv,idx,ni) : -1; }
void vcpe_3d_geometry_changed(const void *v) { if(vcpe_cull_dirty)vcpe_cull_dirty(v); }
void vcpe_3d_cull_reset(void) { if(vcpe_cull_reset)vcpe_cull_reset(); }
uint32_t vcpe_3d_cull_bytes(void) { return vcpe_cull_cache_stats?vcpe_cull_cache_stats()->bytes:0; }
int vcpe_3d_visible_box(const VcpeCullBox *box,const float *model)
{
    if(!autocull || !vcpe_cull_local_planes || !vcpe_cull_box_test) return 1;
    VcpeCullPlanes local;vcpe_cull_local_planes(&local,&camera_planes,model);
    return vcpe_cull_box_test(&local,box)!=0;
}
int vcpe_3d_visible_points(const void *v,uint32_t n,const float *model)
{
    if(!autocull || !vcpe_cull_box_points) return 1;
    VcpeCullBox box;vcpe_cull_box_points(&box,v,n);
    return vcpe_3d_visible_box(&box,model);
}
/* LOD statistics live here, so 3D games that never use LOD meshes need not link vcpe_lod */
int vcpe_lod_stat_full, vcpe_lod_stat_drawn;
static float fog_start, fog_end;
static uint32_t fog_color;

void vcpe_3d_fog(float start, float end, uint32_t color)
{
    fog_start = start;
    fog_end = end;
    fog_color = color;
}

/* view frustum of the current camera, for vcpe_3d_visible */
/* The camera's frustum, kept on the CPU for culling: position, its right/up/forward axes, tan of half the field of
 * view (horizontal fr_th, vertical fr_tv), the matching sphere-margin factors and the near/far distances. */
static float fr_pos[3], fr_right[3], fr_up[3], fr_fwd[3], fr_th, fr_tv, fr_sh, fr_sv, fr_near, fr_far, fr_k;

/* Transform planes into object space: sphere support remains exact even with parent-induced shear. */
int vcpe_3d_visible_local(const float c[3],float radius,const float *m)
{
    if(!autocull) return 1;
    if(vcpe_cull_local_planes && vcpe_cull_sphere_test) {
        VcpeCullPlanes local;vcpe_cull_local_planes(&local,&camera_planes,m);
        return vcpe_cull_sphere_test(&local,c,radius);
    }
    if(!m) return vcpe_3d_visible(c,radius);
    float center[3];
    for(int k=0;k<3;k++)center[k]=m[k*4]*c[0]+m[k*4+1]*c[1]+m[k*4+2]*c[2]+m[k*4+3];
    float s[3]={0},dot01=0,dot02=0,dot12=0;
    for(int k=0;k<3;k++) {
        s[0]+=m[k*4]*m[k*4];s[1]+=m[k*4+1]*m[k*4+1];s[2]+=m[k*4+2]*m[k*4+2];
        dot01+=m[k*4]*m[k*4+1];dot02+=m[k*4]*m[k*4+2];dot12+=m[k*4+1]*m[k*4+2];
    }
    float sum=s[0]+s[1]+s[2],scale=fmaxf(s[0],fmaxf(s[1],s[2]));
    if(fabsf(dot01)+fabsf(dot02)+fabsf(dot12)>0.000001f*sum)scale=sum;
    return vcpe_3d_visible(center,radius*sqrtf(scale));
}

/* Is a sphere (centre c, radius r) at least partly inside the view? Transform its centre into camera space (dot
 * products with the axes), then test it against the near/far planes and the four side planes. Objects that fail are
 * not sent to the GE at all. */
int vcpe_3d_visible(const float c[3], float r)
{
    if(vcpe_cull_sphere_test)return vcpe_cull_sphere_test(&camera_planes,c,r);
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

/* World point to screen pixel (e.g. to place a 2D label over a 3D object); 0 if it is behind the camera. */
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

/* Start drawing 3D with this camera. pos = world position, rot_deg = rotation (Godot YXZ), fov_deg = vertical field
 * of view. */
void vcpe_3d_begin(const float pos[3], const float rot_deg[3], float fov_deg, float znear, float zfar)
{
    vcpe_gfx_flush();
    {
        const float k = GU_PI / 180.0f;
        float cx = cosf(rot_deg[0] * k), sx = sinf(rot_deg[0] * k), cy = cosf(rot_deg[1] * k), sy = sinf(rot_deg[1] * k);
        float cz = cosf(rot_deg[2] * k), sz = sinf(rot_deg[2] * k);
        /* Camera axes for culling, written out from the rotation matrix (forward is minus its third column). */
        /* columns of Ry * Rx * Rz: right, up, back */
        fr_right[0] = cy * cz + sy * sx * sz; fr_right[1] = cx * sz; fr_right[2] = -sy * cz + cy * sx * sz;
        fr_up[0] = -cy * sz + sy * sx * cz; fr_up[1] = cx * cz; fr_up[2] = sy * sz + cy * sx * cz;
        fr_fwd[0] = -sy * cx; fr_fwd[1] = sx; fr_fwd[2] = -cy * cx;
        for (int i = 0; i < 3; i++) fr_pos[i] = pos[i];
        fr_tv = tanf(fov_deg * 0.5f * k);
        fr_th = fr_tv * (float)VCPE_SCR_W / VCPE_SCR_H;
        fr_sv = sqrtf(1 + fr_tv * fr_tv);
        fr_sh = sqrtf(1 + fr_th * fr_th);
        fr_k = (float)VCPE_SCR_H * 0.5f / fr_tv;
        fr_near = znear;
        fr_far = zfar;
    }
    camera_planes.valid=0;
    if(vcpe_cull_camera)vcpe_cull_camera(&camera_planes,fr_pos,fr_right,fr_up,fr_fwd,fr_th,fr_tv,fr_near,fr_far);
    memcpy(model_matrix,identity,sizeof model_matrix);active_3d=1;
    /* Depth: the PSP convention here is reversed (65535 = near, 0 = far), so the test is "greater or equal" and the
     * buffer is cleared to 0 (farthest). The depth mask is GU_FALSE, which on the GE means depth writes are ON. */
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
    /* Alpha test: texels with alpha <= 0x20 are not drawn at all (leaves, fences), and don't write depth either. */
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

    /* Projection: perspective from the field of view and the screen's aspect ratio (480/272). */
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
    vcpe_stat_cull_tests=vcpe_stat_culled=vcpe_stat_cull_tris=vcpe_stat_cull_nodes=0;
    vcpe_lod_stat_full = vcpe_lod_stat_drawn = 0;
}

/* Set the model matrix for the next draws (0 = identity). The engine's matrices are row-major with column vectors;
 * the GU wants them transposed. */
void vcpe_3d_model(const float *m)
{
    memcpy(model_matrix,m?m:identity,sizeof model_matrix);
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

static void submit(const void *verts,const uint16_t *idx,int nidx)
{
    sceGumDrawArray(GU_TRIANGLES, GU_INDEX_16BIT | GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D,
                    nidx, idx, verts);
    vcpe_stat_tris+=nidx/3;
}

/* Test the entire batch first; only intersecting batches need chunk tests. Consecutive retained chunks remain
 * one draw, preserving all original index/alpha order. Entirely rejected batches never bind a texture. */
void vcpe_3d_draw(int page, const void *verts, const uint16_t *idx, int nidx)
{
    if(nidx<=0 || !verts || !idx) return;
    const VcpeCullMesh *mesh=0;
    VcpeCullPlanes local;
    int classification=2;
    const VcpeCullChunk *chunks=0;
    uint32_t count=0,first_visible=0;
    if(autocull && active_3d && vcpe_cull_find && vcpe_cull_local_planes && vcpe_cull_box_test && vcpe_cull_bound && vcpe_cull_chunks) {
        mesh=vcpe_cull_find(verts,idx,(uint32_t)nidx);
        if(mesh) {
            vcpe_stat_cull_tests++;
            vcpe_cull_local_planes(&local,&camera_planes,model_matrix);
            classification=vcpe_cull_box_test(&local,vcpe_cull_bound(mesh));
            if(!classification) {vcpe_stat_culled++;vcpe_stat_cull_tris+=nidx/3;return;}
        }
    }
    if(mesh && classification==1) {
        chunks=vcpe_cull_chunks(mesh,&count);
        for(;first_visible<count;first_visible++) {
            if(vcpe_cull_box_test(&local,&chunks[first_visible].box))break;
            vcpe_stat_cull_tris+=(int)chunks[first_visible].count/3;
        }
        if(first_visible==count) {vcpe_stat_culled++;return;}
    }
    vcpe_gfx_flush();
    if (page >= 0) {
        if (!vcpe_bind_page(page))
            return;
        sceGuEnable(GU_TEXTURE_2D);
    } else {
        vcpe_unbind();
        sceGuDisable(GU_TEXTURE_2D);
    }
    if(mesh && classification==1) {
        uint32_t first=0,run=0;
        for(uint32_t i=first_visible;i<count;i++) {
            int visible=i==first_visible || vcpe_cull_box_test(&local,&chunks[i].box);
            if(visible) {if(!run)first=chunks[i].first;run+=chunks[i].count;}
            else {
                if(run) {submit(verts,idx+first,(int)run);run=0;}
                vcpe_stat_cull_tris+=(int)chunks[i].count/3;
            }
        }
        if(run)submit(verts,idx+first,(int)run);
    } else submit(verts,idx,nidx);
    if (page < 0)
        sceGuEnable(GU_TEXTURE_2D);
}

/* Fade a whole object: blend with fixed factors (a for the object, 1 - a for what is behind) instead of per-texel
 * alpha. See-through objects must not write depth, or they would hide what is drawn behind them later. */
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

/* Back to the 2D defaults: no depth, no fog, no alpha test, blending on, clamped textures. */
void vcpe_3d_end(void)
{
    active_3d=0;
    vcpe_gfx_flush();
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_FOG);
    sceGuEnable(GU_BLEND);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    vcpe_unbind();
}

/* Additive glow: what is behind plus k times the object (light beams, muzzle flashes). */
void vcpe_3d_add(float k)
{
    vcpe_gfx_flush();
    unsigned a = (unsigned)((k < 0 ? 0 : k > 1 ? 1 : k) * 255.0f);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, a * 0x010101u, 0xFFFFFF);   /* dst + k x src */
    sceGuDepthMask(GU_TRUE);
}

/* LOD meshes: the camera of the last vcpe_3d_begin for vcpe_lod_pick (pixels per unit at distance 1 = half the
 * screen height over tan of half the vertical FOV). */
void vcpe_3d_lod_view(VcpeLodView *v, float thresh_px)
{
    for (int i = 0; i < 3; i++)
        v->pos[i] = fr_pos[i];
    v->px_k = fr_k;
    v->thresh_px = thresh_px > 0 ? thresh_px : 1.0f;
    v->down_k = 0.75f;
}

/* One level of a LOD mesh: the shared vertex array with that level's indices. The only GPU call of the LOD path. */
void vcpe_3d_draw_lod(int page, const VcpeLodMesh *m, int level)
{
    if (level < 0) level = 0;
    if (level >= m->nlevels) level = m->nlevels - 1;
    vcpe_lod_stat_full += (int)m->lv[0].nidx / 3;
    vcpe_lod_stat_drawn += vcpe_lod_nidx(m, level) / 3;
    vcpe_3d_draw(page, m->verts, vcpe_lod_idx(m, level), vcpe_lod_nidx(m, level));
}
