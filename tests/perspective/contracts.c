#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "vcpe_perspective.h"
#include "vcpe_sector.h"
#include "vcpe_math.h"
static VcpePerspectiveTriangle storage[256];
static VcpePerspectiveCamera cam={{0,1,0},0,0,70,.1f,50,480,272};
static VcpeVtx card[4]={{0,0,0xffffffff,-1,2,-4},{1,0,0xffffffff,1,2,-4},
 {1,1,0xffffffff,1,0,-4},{0,1,0xffffffff,-1,0,-4}};
static int finite_bits(float f){uint32_t b;memcpy(&b,&f,4);return (b&0x7f800000u)!=0x7f800000u;}
static void check_queue(VcpePerspective *p) {
 for(unsigned i=0;i<p->count;i++)for(int j=0;j<3;j++) {
  VcpeVtx *v=&p->triangles[i].v[j];
  assert(finite_bits(v->x)&&finite_bits(v->y)&&finite_bits(v->u)&&finite_bits(v->v));
  assert(v->x>=0&&v->x<=480&&v->y>=0&&v->y<=272&&v->z==0);
 }
}
int main(void) {
 vcpe_math_init(); VcpePerspective p;
 assert(!vcpe_perspective_begin(&p,&cam,storage,256));
 assert(vcpe_perspective_card(&p,0,card)==2);check_queue(&p);
 assert(p.triangles[0].v[0].x<240 && p.triangles[0].v[0].y<136);
 float nan;uint32_t nan_bits=0x7fc00000u;memcpy(&nan,&nan_bits,4);
 VcpePerspectiveCamera bad=cam;bad.fov=nan;
 assert(vcpe_perspective_begin(&p,&bad,storage,256)==-1);
 assert(!vcpe_perspective_begin(&p,&cam,storage,1));
 assert(!vcpe_perspective_card(&p,0,card)&&p.count==0&&p.dropped==1);
 assert(!vcpe_perspective_begin(&p,&cam,storage,256));
 VcpeVtx near[4];memcpy(near,card,sizeof near);near[0].z=near[3].z=.2f;
 assert(vcpe_perspective_card(&p,1,near)>0);check_queue(&p);
 for(int n=0;n<1000;n++) {
  assert(!vcpe_perspective_begin(&p,&cam,storage,256));
  VcpeVtx v[4];memcpy(v,card,sizeof v);
  for(int j=0;j<4;j++){v[j].x+=vcpe_sin_deg(n*17+j*31)*20;
   v[j].y+=vcpe_cos_deg(n*13+j*27)*20;v[j].z+=vcpe_sin_deg(n*19+j*23)*70;}
  vcpe_perspective_card(&p,0,v);check_queue(&p);
 }
 assert(!vcpe_perspective_begin(&p,&cam,storage,256));
 vcpe_perspective_card(&p,2,card);
 VcpeVtx far[4];memcpy(far,card,sizeof far);for(int j=0;j<4;j++)far[j].z=-10;
 vcpe_perspective_card(&p,3,far);vcpe_perspective_sort(&p);
 assert(p.triangles[0].page==3&&p.triangles[3].page==2);
 assert(vcpe_perspective_view(0,-1,0,0,0,8)==0);
 assert(vcpe_perspective_view(-1,0,0,0,0,8)==2);
 assert(vcpe_perspective_view(0,1,0,0,0,8)==4);
 assert(vcpe_perspective_view(1,0,0,0,0,8)==6);
 assert(vcpe_perspective_view(0,-1,0,0,360,8)==0);
 VcpeSectorCell cells[]={{-2,-4,2,0,0,3},{-2,-8,2,-4,0,3},{-2,-12,2,-8,0,3}};
 VcpeSectorPortal portals[]={{0,1,1,-.8f,-4,.8f,-4},{1,2,0,-.8f,-8,.8f,-8}};
 VcpeSectors s={cells,3,portals,2};assert(!vcpe_sector_validate(&s));
 assert(vcpe_sector_connected(&s,0)==3);
 VcpeSectorBody b={0,0,-1,.2f,1.7f,.3f,.5f,0};
 for(int i=0;i<80;i++)assert(vcpe_sector_move(&s,&b,0,-.1f)>=0);
 assert(b.cell==1 && b.z> -7.81f);
 portals[1].open=1; assert(vcpe_sector_connected(&s,0)==7);
 for(int i=0;i<20;i++)vcpe_sector_move(&s,&b,0,-.1f);
 assert(b.cell==2);
 b=(VcpeSectorBody){1.5f,0,-3,.2f,1.7f,.3f,.5f,0};
 for(int i=0;i<20;i++)vcpe_sector_move(&s,&b,0,-.1f);
 assert(b.cell==0 && b.z> -3.81f);
 VcpeSectorBody before=b; assert(vcpe_sector_move(&s,&b,100,0)==-1);
 assert(!memcmp(&b,&before,sizeof b));
 cells[1].ceiling=1; b=(VcpeSectorBody){0,0,-3,.2f,1.7f,.3f,.5f,0};
 for(int i=0;i<20;i++)vcpe_sector_move(&s,&b,0,-.1f);
 assert(b.cell==0); cells[1].ceiling=3;
 cells[1].floor=.6f;
 for(int i=0;i<20;i++)vcpe_sector_move(&s,&b,0,-.1f);
 assert(b.cell==0); cells[1].floor=0;
 /* A permitted drop must not trap the body within its radius of the entrance. */
 cells[0].floor=.4f;b=(VcpeSectorBody){0,.4f,-3.5f,.2f,1.7f,.3f,.5f,0};
 for(int i=0;i<20;i++)vcpe_sector_move(&s,&b,0,-.1f);
 assert(b.cell==1&&b.z< -5&&b.y==0);cells[0].floor=0;
 portals[0].z0=-3; assert(vcpe_sector_validate(&s)==-1);
 puts("PASS projection, six-plane clipping, fixed capacity, deterministic painter order, 8 views, portals, collision, door, step/headroom, tunnelling rejection");
}
