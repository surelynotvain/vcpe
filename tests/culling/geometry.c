#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "vcpe_cull.h"
typedef struct {float u,v;uint32_t color;float p[3];} V;
static uint32_t seed=173;
static float random_float(float lo,float hi) {seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return lo+(hi-lo)*(seed&65535)/65535.0f;}
static float unusual(uint32_t bits) {float f;memcpy(&f,&bits,4);return f;}
int main(void)
{
    float zero[3]={0},r[3]={1,0,0},u[3]={0,1,0},f[3]={0,0,-1};
    VcpeCullPlanes view;vcpe_cull_camera(&view,zero,r,u,f,1,1,1,100);assert(view.valid);
    VcpeCullBox b={{0,0,-5},{1,1,1},1};assert(vcpe_cull_box_test(&view,&b)==2);
    b.center[0]=100;assert(!vcpe_cull_box_test(&view,&b));
    b.center[0]=0;b.center[2]=5;assert(!vcpe_cull_box_test(&view,&b));
    b.center[2]=-150;assert(!vcpe_cull_box_test(&view,&b));
    b.center[2]=-2;b.extent[2]=1;assert(vcpe_cull_box_test(&view,&b)); /* near tangent */
    b.center[2]=-101;assert(vcpe_cull_box_test(&view,&b)); /* far tangent */
    b.center[2]=-5;b.center[0]=6;b.extent[0]=1;b.extent[2]=0;assert(vcpe_cull_box_test(&view,&b));
    b.extent[0]=-1;assert(vcpe_cull_box_test(&view,&b));
    b.extent[0]=1;b.center[0]=unusual(0x7FC00000);assert(vcpe_cull_box_test(&view,&b));
    b.center[0]=unusual(0x7F800000);assert(vcpe_cull_box_test(&view,&b));
    VcpeCullPlanes bad;float invalid[3]={0};vcpe_cull_camera(&bad,zero,invalid,u,f,1,1,1,100);assert(!bad.valid);
    vcpe_cull_camera(&bad,zero,r,u,f,0,1,1,100);assert(!bad.valid);
    /* Independent oracle transforms all eight corners to world space, then evaluates the world planes.
     * Signed/zero scales, rotations, parent-like shears, translations and moving cameras are randomized. */
    int rejected=0;
    for(int trial=0;trial<20000;trial++) {
        float yaw=random_float(-3.14f,3.14f),pitch=random_float(-1.4f,1.4f),cp=cosf(pitch),sp=sinf(pitch);
        float right[3]={cosf(yaw),0,-sinf(yaw)},up[3]={sinf(yaw)*sp,cp,cosf(yaw)*sp};
        float forward[3]={-sinf(yaw)*cp,sp,-cosf(yaw)*cp};
        float pos[3]={random_float(-20,20),random_float(-20,20),random_float(-20,20)};
        vcpe_cull_camera(&view,pos,right,up,forward,random_float(.2f,2),random_float(.2f,2),.1f,100);assert(view.valid);
        float m[16]={0};m[15]=1;
        for(int i=0;i<3;i++)for(int j=0;j<3;j++)m[i*4+j]=random_float(-3,3);
        for(int k=0;k<3;k++)m[k*4+3]=random_float(-200,200);
        for(int k=0;k<3;k++){b.center[k]=random_float(-10,10);b.extent[k]=random_float(0,8);}b.valid=1;
        VcpeCullPlanes local;vcpe_cull_local_planes(&local,&view,m);assert(local.valid);
        int actual=vcpe_cull_box_test(&local,&b),oracle_out=0;
        for(int p=0;p<6;p++) {
            double maximum=-1e300;
            for(int corner=0;corner<8;corner++) {
                double x[3],w[3];for(int k=0;k<3;k++)x[k]=b.center[k]+((corner>>k)&1?1:-1)*b.extent[k];
                for(int k=0;k<3;k++)w[k]=m[k*4]*x[0]+m[k*4+1]*x[1]+m[k*4+2]*x[2]+m[k*4+3];
                double distance=view.p[p][0]*w[0]+view.p[p][1]*w[1]+view.p[p][2]*w[2]+view.p[p][3];
                if(distance>maximum)maximum=distance;
            }
            if(maximum < 0)oracle_out=1;
        }
        if(!actual){assert(oracle_out);rejected++;}
        /* A point from the local sphere's support in the plane normal must not be falsely rejected. */
        float center[3]={0,0,0};float radius=2;
        int sphere=vcpe_cull_sphere_test(&local,center,radius);
        VcpeCullPrepared prepared;vcpe_cull_prepare_planes(&prepared,&local);
        assert(vcpe_cull_sphere_prepared(&prepared,center,radius)==sphere);
        float invalid_center[3]={unusual(0x7fc00000),0,0};
        assert(vcpe_cull_sphere_prepared(&prepared,invalid_center,radius));
        assert(vcpe_cull_sphere_prepared(&prepared,center,-1));
        VcpeCullPrepared invalid_prepared={0};assert(vcpe_cull_sphere_prepared(&invalid_prepared,center,radius));
        if(!sphere) {
            int outside=0;
            for(int p=0;p<6;p++) {
                double norm=sqrt((double)local.p[p][0]*local.p[p][0]+(double)local.p[p][1]*local.p[p][1]+(double)local.p[p][2]*local.p[p][2]);
                if(local.p[p][3]+radius*norm<0)outside=1;
            }
            assert(outside);
        }
    }
    assert(rejected>10000);
    float m[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};m[12]=.1f;
    vcpe_cull_local_planes(&bad,&view,m);assert(!bad.valid);
    /* Cache dedup, all shared LOD lists, dynamic refit, retained index order, bounds validation and budget fallback. */
    V verts[6]={0};uint16_t idx[6]={0,1,2,3,4,5},lod[3]={0,2,5};
    for(int i=0;i<6;i++){verts[i].p[0]=(float)i;verts[i].p[2]=-5;}
    vcpe_cull_limits(8192,8);
    assert(!vcpe_cull_prepare(verts,6,idx,6));uint32_t bytes=vcpe_cull_cache_stats()->bytes;
    assert(!vcpe_cull_prepare(verts,6,idx,6) && vcpe_cull_cache_stats()->bytes==bytes);
    assert(!vcpe_cull_prepare(verts,6,lod,3));
    const VcpeCullMesh *mesh=vcpe_cull_find(verts,idx,6);assert(mesh);
    const VcpeCullBox *bounds=vcpe_cull_bound(mesh);assert(bounds->valid && bounds->center[0]==2.5f);
    for(int i=0;i<6;i++)verts[i].p[0]+=100;
    vcpe_cull_dirty(verts);mesh=vcpe_cull_find(verts,idx,6);assert(vcpe_cull_bound(mesh)->center[0]==102.5f);
    assert(vcpe_cull_bound(vcpe_cull_find(verts,lod,3))->center[0]==102.5f && vcpe_cull_cache_stats()->refits==2);
    idx[5]=7;vcpe_cull_dirty(verts);mesh=vcpe_cull_find(verts,idx,6);assert(!vcpe_cull_bound(mesh)->valid);idx[5]=5;
    vcpe_cull_forget(verts);assert(!vcpe_cull_cache_stats()->bytes && !vcpe_cull_cache_stats()->entries);
    vcpe_cull_limits(1,8);assert(vcpe_cull_prepare(verts,6,idx,6)<0 && !vcpe_cull_find(verts,idx,6));
    vcpe_cull_limits(8192,8);idx[0]=99;assert(vcpe_cull_prepare(verts,6,idx,6)<0 && !vcpe_cull_cache_stats()->bytes);
    vcpe_cull_reset();
    printf("PASS culling geometry: six planes/edges, invalid inputs, 20000 affine/camera oracles (%d rejects), mutable LODs, cache lifetime, index validation and budget fallback\n",rejected);
}
