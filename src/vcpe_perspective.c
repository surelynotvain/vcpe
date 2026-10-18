#include "vcpe_perspective.h"
#include "vcpe_math.h"
#include <string.h>
#include <stdlib.h>

/* Bit check remains valid with PSP builds using -ffast-math. */
static int finite_float(float f)
{ uint32_t bits; memcpy(&bits,&f,4); return (bits&0x7f800000u)!=0x7f800000u; }
static int sane(float f) { return finite_float(f) && f>-1000000 && f<1000000; }
int vcpe_perspective_begin(VcpePerspective *p,const VcpePerspectiveCamera *c,
                          VcpePerspectiveTriangle *storage,unsigned capacity)
{
    if(!p) return -1;
    memset(p,0,sizeof *p);
    if(!c || !storage || !capacity || capacity>65536 || c->width<1 || c->width>4096 ||
       c->height<1 || c->height>4096 || !sane(c->yaw) || !sane(c->pitch) ||
       !sane(c->fov) || c->fov<10 || c->fov>150 || !sane(c->near_z) ||
       !sane(c->far_z) || c->near_z<.01f || c->far_z<=c->near_z) return -1;
    for(int k=0;k<3;k++) if(!sane(c->pos[k])) return -1;
    p->camera=*c; p->triangles=storage; p->capacity=capacity;
    float cy=vcpe_cos_deg(c->yaw),sy=vcpe_sin_deg(c->yaw);
    float cp=vcpe_cos_deg(c->pitch),sp=vcpe_sin_deg(c->pitch);
    p->right[0]=cy; p->right[2]=-sy;
    p->up[0]=sy*sp; p->up[1]=cp; p->up[2]=cy*sp;
    p->forward[0]=-sy*cp; p->forward[1]=sp; p->forward[2]=-cy*cp;
    p->focal=c->height*.5f*vcpe_cos_deg(c->fov*.5f)/vcpe_sin_deg(c->fov*.5f);
    p->valid=1; return 0;
}
/* View-space Sutherland-Hodgman clipping. Every input triangle grows to at most
 * nine vertices through six planes. Generous fixed scratch space, no heap. */
static float distance_plane(const VcpePerspective *p,const VcpeVtx *v,int plane)
{
    switch(plane) {
    case 0:return v->z-p->camera.near_z;
    case 1:return p->camera.far_z-v->z;
    case 2:return v->z*p->camera.width*.5f+v->x*p->focal;
    case 3:return v->z*p->camera.width*.5f-v->x*p->focal;
    case 4:return v->z*p->camera.height*.5f+v->y*p->focal;
    default:return v->z*p->camera.height*.5f-v->y*p->focal;
    }
}
static VcpeVtx mix_vertex(VcpeVtx a,VcpeVtx b,float t)
{
    VcpeVtx v;
    v.x=a.x+(b.x-a.x)*t; v.y=a.y+(b.y-a.y)*t; v.z=a.z+(b.z-a.z)*t;
    v.u=a.u+(b.u-a.u)*t; v.v=a.v+(b.v-a.v)*t;
    v.color=0;
    for(int k=0;k<32;k+=8) {
        float x=(float)((a.color>>k)&255),y=(float)((b.color>>k)&255);
        v.color|=(uint32_t)(x+(y-x)*t+.5f)<<k;
    }
    return v;
}
static void emit(VcpePerspective *p,int page,const VcpeVtx *a,const VcpeVtx *b,const VcpeVtx *c)
{
    VcpePerspectiveTriangle *t=&p->triangles[p->count];
    t->page=page;t->order=p->count;t->depth=(a->z+b->z+c->z)/3;
    t->v[0]=*a;t->v[1]=*b;t->v[2]=*c;
    for(int j=0;j<3;j++) {
        VcpeVtx *v=&t->v[j];float k=p->focal/v->z;
        v->x=vcpe_clampf(p->camera.width*.5f+v->x*k,0,(float)p->camera.width);
        v->y=vcpe_clampf(p->camera.height*.5f-v->y*k,0,(float)p->camera.height);
        v->z=0;
    }
    p->count++;p->sorted=0;
}
static int triangle(VcpePerspective *p,const VcpeVtx input[3],int page)
{
    VcpeVtx a[16],b[16]; int n=3;
    memcpy(a,input,3*sizeof *a);
    for(int plane=0;plane<6 && n;plane++) {
        int out=0;
        for(int i=0;i<n;i++) {
            VcpeVtx s=a[i],e=a[(i+1)%n];
            float ds=distance_plane(p,&s,plane),de=distance_plane(p,&e,plane);
            if((ds>=0)!=(de>=0)) b[out++]=mix_vertex(s,e,ds/(ds-de));
            if(de>=0) b[out++]=e;
        }
        n=out; memcpy(a,b,(size_t)n*sizeof *a);
    }
    if(n<3) return 0;
    if(p->count+(unsigned)(n-2)>p->capacity) return -1;
    for(int i=1;i<n-1;i++) {
        emit(p,page,&a[0],&a[i],&a[i+1]);
    }
    return n-2;
}
int vcpe_perspective_card(VcpePerspective *p,int page,const VcpeVtx v[4])
{
    if(!p || !p->valid || !v) return 0;
    VcpeVtx view[4]; p->cards++;
    for(int i=0;i<4;i++) {
        if(!sane(v[i].x)||!sane(v[i].y)||!sane(v[i].z)||!sane(v[i].u)||!sane(v[i].v)) {
            p->rejected++; return 0;
        }
        view[i]=v[i];
        float d[3]={v[i].x-p->camera.pos[0],v[i].y-p->camera.pos[1],v[i].z-p->camera.pos[2]};
        view[i].x=d[0]*p->right[0]+d[1]*p->right[1]+d[2]*p->right[2];
        view[i].y=d[0]*p->up[0]+d[1]*p->up[1]+d[2]*p->up[2];
        view[i].z=d[0]*p->forward[0]+d[1]*p->forward[1]+d[2]*p->forward[2];
    }
    /* Four corner outcodes reject hidden cards without copying/rotating two
     * triangles through six scratch buffers. Fully inside cards project directly. */
    unsigned any=0,all=63;
    for(int i=0;i<4;i++) {
        unsigned code=0;
        for(int plane=0;plane<6;plane++)if(distance_plane(p,&view[i],plane)<0)code|=1u<<plane;
        any|=code;all&=code;
    }
    if(all){p->rejected++;return 0;}
    if(!any) {
        if(p->capacity-p->count<2){p->dropped++;return 0;}
        emit(p,page,&view[0],&view[1],&view[2]);emit(p,page,&view[0],&view[2],&view[3]);return 2;
    }
    unsigned start=p->count;
    VcpeVtx a[3]={view[0],view[1],view[2]},b[3]={view[0],view[2],view[3]};
    if(triangle(p,a,page)<0 || triangle(p,b,page)<0) {
        p->count=start; p->dropped++; return 0;
    }
    if(p->count==start) p->rejected++;
    return (int)(p->count-start);
}
int vcpe_perspective_view(float cx,float cz,float ax,float az,float heading,int views)
{
    if(views<1 || views>32 || !sane(cx)||!sane(cz)||!sane(ax)||!sane(az)||!sane(heading)) return 0;
    float angle=vcpe_atan2(-(cx-ax),-(cz-az))*(180/VCPE_PI)-heading;
    angle=vcpe_angle_diff_deg(angle,0);
    if(angle<0) angle+=360;
    int i=(int)(angle*(views/360.0f)+.5f);
    return i==views?0:i;
}
int vcpe_perspective_sprite(VcpePerspective *p,int page,const float feet[3],float w,float h,
                           const float uv[4],uint32_t color)
{
    if(!p || !p->valid || !feet || !uv || !sane(w)||!sane(h)||w<=0||h<=0) return 0;
    float x=p->right[0]*w*.5f,z=p->right[2]*w*.5f;
    VcpeVtx v[4]={
        {uv[0],uv[1],color,feet[0]-x,feet[1]+h,feet[2]-z},
        {uv[2],uv[1],color,feet[0]+x,feet[1]+h,feet[2]+z},
        {uv[2],uv[3],color,feet[0]+x,feet[1],feet[2]+z},
        {uv[0],uv[3],color,feet[0]-x,feet[1],feet[2]-z}};
    return vcpe_perspective_card(p,page,v);
}
static int far_first(const void *a,const void *b)
{
    const VcpePerspectiveTriangle *x=a,*y=b;
    if(x->depth>y->depth) return -1;
    if(x->depth<y->depth) return 1;
    return x->order<y->order?-1:x->order>y->order;
}
void vcpe_perspective_sort(VcpePerspective *p)
{ if(p && p->valid && !p->sorted) {qsort(p->triangles,p->count,sizeof *p->triangles,far_first);p->sorted=1;} }
