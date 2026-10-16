/* Culling must keep nonfinite checks even when the rest of a game uses -ffast-math.
 * Clang's precise pragma is documented in https://clang.llvm.org/docs/LanguageExtensions.html#float-control-pragma
 * GCC keeps the other PSP fast-math choices but must not assume every float is finite. */
#if defined(__clang__)
#pragma float_control(precise, on)
#elif defined(__GNUC__)
#pragma GCC optimize ("no-finite-math-only")
#endif
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "vcpe_cull.h"

typedef struct { float u, v; uint32_t color; float xyz[3]; } Vertex;
typedef char VertexSizeMustBe24[sizeof(Vertex) == 24 ? 1 : -1];
struct VcpeCullMesh {
    struct VcpeCullMesh *next;
    const Vertex *verts;
    const uint16_t *indices;
    uint32_t nverts, nidx, nchunks, bytes;
    int dirty;
    VcpeCullBox box;
    VcpeCullChunk chunks[];
};
/* A 1 KiB bucket table on PSP; entries/chunks grow only during prepare, under an explicit heap cap. */
static VcpeCullMesh *buckets[256];
static uint32_t budget = 512 * 1024, chunk_tris = 128;
static VcpeCullCacheStats stats;

/* isfinite may be optimized away with the PSP's -ffast-math. Inspect IEEE-754 bits instead. */
static int finite_float(float f)
{
    uint32_t bits; memcpy(&bits, &f, sizeof bits);
    return (bits & 0x7F800000u) != 0x7F800000u;
}
static int finite_array(const float *f, unsigned n)
{
    for (unsigned i=0; i<n; i++) if (!finite_float(f[i])) return 0;
    return 1;
}
static unsigned bucket(const void *p) { uintptr_t v=(uintptr_t)p; return (unsigned)((v>>4)^(v>>13)) & 255; }

void vcpe_cull_camera(VcpeCullPlanes *o, const float pos[3], const float r[3], const float u[3],
                      const float f[3], float th, float tv, float zn, float zf)
{
    memset(o,0,sizeof *o);
    if (!pos || !r || !u || !f || !finite_array(pos,3) || !finite_array(r,3) || !finite_array(u,3) ||
        !finite_array(f,3) || !finite_float(th) || !finite_float(tv) || !finite_float(zn) || !finite_float(zf) ||
        th<=0 || tv<=0 || zn<0 || zf<=zn) return;
    const float *axes[3]={r,u,f};
    for(int a=0;a<3;a++)for(int b=a;b<3;b++) {
        float dot=0;for(int k=0;k<3;k++)dot+=axes[a][k]*axes[b][k];
        if(fabsf(dot-(a==b?1.0f:0.0f))>0.001f)return;
    }
    for (int k=0; k<3; k++) {
        o->p[0][k]=f[k]; o->p[1][k]=-f[k];
        o->p[2][k]=th*f[k]+r[k]; o->p[3][k]=th*f[k]-r[k];
        o->p[4][k]=tv*f[k]+u[k]; o->p[5][k]=tv*f[k]-u[k];
    }
    for (int p=0; p<6; p++) {
        float *n=o->p[p];
        n[3]=-(n[0]*pos[0]+n[1]*pos[1]+n[2]*pos[2]);
    }
    o->p[0][3]-=zn; o->p[1][3]+=zf;
    o->valid=finite_array(&o->p[0][0],24);
}

void vcpe_cull_local_planes(VcpeCullPlanes *o, const VcpeCullPlanes *view, const float *m)
{
    VcpeCullPlanes result=*view;
    if (result.valid && m) {
        if (!finite_array(m,16) || m[12]!=0 || m[13]!=0 || m[14]!=0 || m[15]!=1) result.valid=0;
        else for (int p=0; p<6; p++) for (int k=0; k<4; k++)
            result.p[p][k]=view->p[p][0]*m[k]+view->p[p][1]*m[4+k]+view->p[p][2]*m[8+k]+view->p[p][3]*m[12+k];
        if (!finite_array(&result.p[0][0],24)) result.valid=0;
    }
    *o=result;
}

static float margin(const float *p, const float *c, float support)
{
    return 0.00002f * (1 + fabsf(p[3]) + fabsf(p[0]*c[0]) + fabsf(p[1]*c[1]) + fabsf(p[2]*c[2]) + support);
}
int vcpe_cull_box_test(const VcpeCullPlanes *planes, const VcpeCullBox *b)
{
    if (!planes || !planes->valid || !finite_array(&planes->p[0][0],24) || !b || !b->valid ||
        !finite_array(b->center,3) || !finite_array(b->extent,3) || b->extent[0]<0 || b->extent[1]<0 || b->extent[2]<0) return 1;
    int inside=1;
    for (int i=0; i<6; i++) {
        const float *p=planes->p[i];
        float d=p[0]*b->center[0]+p[1]*b->center[1]+p[2]*b->center[2]+p[3];
        float r=fabsf(p[0])*b->extent[0]+fabsf(p[1])*b->extent[1]+fabsf(p[2])*b->extent[2];
        float e=margin(p,b->center,r);
        if (!finite_float(d) || !finite_float(r) || !finite_float(e) || r<0) return 1;
        if (d+r < -e) return 0;
        if (d-r < e) inside=0;
    }
    return inside ? 2 : 1;
}
int vcpe_cull_sphere_test(const VcpeCullPlanes *planes, const float c[3], float radius)
{
    if (!planes || !planes->valid || !finite_array(&planes->p[0][0],24) || !c || !finite_array(c,3) || !finite_float(radius) || radius<0) return 1;
    for (int i=0; i<6; i++) {
        const float *p=planes->p[i];
        float d=p[0]*c[0]+p[1]*c[1]+p[2]*c[2]+p[3];
        float r=radius*sqrtf(p[0]*p[0]+p[1]*p[1]+p[2]*p[2]);
        float e=margin(p,c,r);
        if (!finite_float(d) || !finite_float(r) || !finite_float(e)) return 1;
        if (d+r < -e) return 0;
    }
    return 1;
}
static void box_finish(VcpeCullBox *b, const float lo[3], const float hi[3])
{
    b->valid=1;
    for (int k=0; k<3; k++) { b->center[k]=lo[k]*0.5f+hi[k]*0.5f; b->extent[k]=(hi[k]-lo[k])*0.5f; }
    if (!finite_array(b->center,3) || !finite_array(b->extent,3)) b->valid=0;
}
void vcpe_cull_box_points(VcpeCullBox *o, const void *verts, uint32_t count)
{
    memset(o,0,sizeof *o);
    if (!verts || !count) return;
    const Vertex *v=verts;
    float lo[3],hi[3]; memcpy(lo,v[0].xyz,12); memcpy(hi,lo,12);
    for (uint32_t i=0; i<count; i++) {
        if (!finite_array(v[i].xyz,3)) return;
        for (int k=0;k<3;k++) { if(v[i].xyz[k]<lo[k])lo[k]=v[i].xyz[k]; if(v[i].xyz[k]>hi[k])hi[k]=v[i].xyz[k]; }
    }
    box_finish(o,lo,hi);
}
static int refit(VcpeCullMesh *m)
{
    float all_lo[3]={0},all_hi[3]={0}; int all_valid=1;
    for (uint32_t c=0;c<m->nchunks;c++) {
        VcpeCullChunk *chunk=&m->chunks[c]; float lo[3]={0},hi[3]={0}; int valid=1;
        for (uint32_t i=0;i<chunk->count;i++) {
            uint16_t index=m->indices[chunk->first+i];
            if(index>=m->nverts) { for(uint32_t j=0;j<m->nchunks;j++)m->chunks[j].box.valid=0; m->box.valid=0; return -1; }
            const float *p=m->verts[index].xyz;
            if(!finite_array(p,3))valid=0;
            for(int k=0;k<3;k++) {
                if(!i || p[k]<lo[k])lo[k]=p[k];
                if(!i || p[k]>hi[k])hi[k]=p[k];
            }
        }
        box_finish(&chunk->box,lo,hi); chunk->box.valid &= valid;
        all_valid &= chunk->box.valid;
        for(int k=0;k<3;k++) { if(!c || lo[k]<all_lo[k])all_lo[k]=lo[k]; if(!c || hi[k]>all_hi[k])all_hi[k]=hi[k]; }
    }
    box_finish(&m->box,all_lo,all_hi); m->box.valid &= all_valid;
    m->dirty=0; return 0;
}
int vcpe_cull_prepare(const void *verts, uint32_t nverts, const uint16_t *idx, uint32_t nidx)
{
    if(!verts || !idx || !nverts || nverts>65536 || !nidx || nidx%3) return -1;
    for(VcpeCullMesh *m=buckets[bucket(verts)];m;m=m->next)
        if(m->verts==verts && m->indices==idx && m->nidx==nidx) {
            if(m->nverts!=nverts) {m->nverts=nverts;m->dirty=1;}
            return 0;
        }
    uint32_t indices_per_chunk=chunk_tris*3;
    uint32_t chunks=1+(nidx-1)/indices_per_chunk;
    if(chunks>(UINT32_MAX-sizeof(VcpeCullMesh))/sizeof(VcpeCullChunk)) {stats.fallbacks++;return -1;}
    uint32_t bytes=(uint32_t)sizeof(VcpeCullMesh)+chunks*(uint32_t)sizeof(VcpeCullChunk);
    if(bytes>budget || stats.bytes>budget-bytes) {stats.fallbacks++;return -1;}
    VcpeCullMesh *m=malloc(bytes);
    if(!m) {stats.fallbacks++;return -1;}
    memset(m,0,sizeof *m);m->verts=verts;m->indices=idx;m->nverts=nverts;m->nidx=nidx;m->nchunks=chunks;m->bytes=bytes;
    for(uint32_t i=0,first=0;i<chunks;i++) {
        uint32_t n=nidx-first;if(n>indices_per_chunk)n=indices_per_chunk;
        m->chunks[i].first=first;m->chunks[i].count=n;first+=n;
    }
    if(refit(m)<0) {free(m);stats.fallbacks++;return -1;}
    unsigned b=bucket(verts);m->next=buckets[b];buckets[b]=m;
    stats.bytes+=bytes;stats.entries++;if(stats.bytes>stats.peak_bytes)stats.peak_bytes=stats.bytes;
    return 0;
}
const VcpeCullMesh *vcpe_cull_find(const void *verts,const uint16_t *idx,uint32_t nidx)
{
    for(VcpeCullMesh *m=buckets[bucket(verts)];m;m=m->next)
        if(m->verts==verts && m->indices==idx && m->nidx==nidx) {
            if(m->dirty) {refit(m);stats.refits++;} return m;
        }
    return 0;
}
const VcpeCullBox *vcpe_cull_bound(const VcpeCullMesh *m) {return m?&m->box:0;}
const VcpeCullChunk *vcpe_cull_chunks(const VcpeCullMesh *m,uint32_t *n) {if(n)*n=m?m->nchunks:0;return m?m->chunks:0;}
void vcpe_cull_dirty(const void *verts)
{
    for(VcpeCullMesh *m=buckets[bucket(verts)];m;m=m->next)if(m->verts==verts)m->dirty=1;
}
void vcpe_cull_forget(const void *verts)
{
    VcpeCullMesh **link=&buckets[bucket(verts)];
    while(*link) {
        VcpeCullMesh *m=*link;
        if(m->verts==verts) {*link=m->next;stats.bytes-=m->bytes;stats.entries--;free(m);}
        else link=&m->next;
    }
}
void vcpe_cull_reset(void)
{
    for(unsigned b=0;b<256;b++) {
        VcpeCullMesh *m=buckets[b];while(m){VcpeCullMesh *next=m->next;free(m);m=next;}buckets[b]=0;
    }
    memset(&stats,0,sizeof stats);
}
void vcpe_cull_limits(uint32_t bytes,uint32_t triangles)
{
    vcpe_cull_reset();budget=bytes;chunk_tris=triangles?triangles:128;
    if(chunk_tris>65536)chunk_tris=65536;
    if(chunk_tris<8)chunk_tris=8;
    chunk_tris=(chunk_tris+7)&~7u; /* chunk starts preserve the caller's 16-byte index-buffer alignment */
}
const VcpeCullCacheStats *vcpe_cull_cache_stats(void) {return &stats;}
