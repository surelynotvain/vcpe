#include "vcpe_raycast.h"
#include "vcpe_math.h"
#include <string.h>
static int sane(float f)
{
    uint32_t b;memcpy(&b,&f,4);
    /* Integer magnitude test cannot be folded away under -ffast-math when
     * combined with ordered float comparisons (NaNs fail those only in strict). */
    const float limit=100000.0f;uint32_t max;memcpy(&max,&limit,4);
    return (b&0x7fffffffu)<max;
}
int vcpe_raycast(const VcpeRayView *v,const VcpeRayWall *walls,int count,int strip,VcpeRayColumn *out,int cap)
{
    if(!v||!walls||!out||count<0||count>4096||strip<1||strip>32||v->width<1||v->width>480||
       v->height<1||v->height>272||!sane(v->x)||!sane(v->z)||!sane(v->eye)||!sane(v->yaw)||
       !sane(v->pitch)||v->pitch< -60||v->pitch>60||!sane(v->fov)||v->fov<10||v->fov>150||
       !sane(v->near_z)||!sane(v->far_z)||v->near_z<.01f||v->far_z<=v->near_z)return -1;
    int n=(v->width+strip-1)/strip;if(cap<n)return -1;
    for(int i=0;i<count;i++){
        const VcpeRayWall *w=&walls[i];
        if(!sane(w->x0)||!sane(w->x1)||!sane(w->z0)||!sane(w->z1)||!sane(w->bottom)||
           !sane(w->top)||w->top<=w->bottom)return -1;
    }
    float sy=vcpe_sin_deg(v->yaw),cy=vcpe_cos_deg(v->yaw);
    float focal=v->height*.5f*vcpe_cos_deg(v->fov*.5f)/vcpe_sin_deg(v->fov*.5f);
    float horizon=v->height*.5f+vcpe_sin_deg(v->pitch)/vcpe_cos_deg(v->pitch)*focal;
    for(int i=0;i<n;i++){
        VcpeRayColumn *c=&out[i];c->wall=-1;c->depth=v->far_z;c->tex_u=c->top=c->bottom=0;
        float screen_x=(i*strip+strip*.5f-v->width*.5f)/focal;
        float dx=-sy+cy*screen_x,dz=-cy-sy*screen_x;
        for(int j=0;j<count;j++){
            const VcpeRayWall *w=&walls[j];
            float ex=w->x1-w->x0,ez=w->z1-w->z0,rx=w->x0-v->x,rz=w->z0-v->z;
            float den=dx*ez-dz*ex;
            if(den>-.00001f&&den<.00001f)continue;
            float inv=1/den,t=(rx*ez-rz*ex)*inv,u=(rx*dz-rz*dx)*inv;
            if(t<v->near_z||t>=c->depth||u<0||u>1)continue;
            c->wall=j;c->depth=t;c->tex_u=u;
            c->top=horizon+(v->eye-w->top)*focal/t;
            c->bottom=horizon+(v->eye-w->bottom)*focal/t;
        }
    }
    return n;
}
