#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "vcpe_raycast.h"
#include "vcpe_math.h"
int main(void){
 vcpe_math_init();VcpeRayColumn col[120];
 VcpeRayWall wall={-20,-4,20,-4,0,3,0,0xffffffff};
 VcpeRayView view={0,0,1.5f,0,0,64,.1f,25,480,272};
 assert(vcpe_raycast(&view,&wall,1,4,col,120)==120);
 for(int i=0;i<120;i++){
  assert(col[i].wall==0&&col[i].depth>3.999f&&col[i].depth<4.001f);
  assert(col[i].tex_u>=0&&col[i].tex_u<=1&&col[i].top<136&&col[i].bottom>136);
 }
 view.yaw=180;assert(vcpe_raycast(&view,&wall,1,4,col,120)==120);
 for(int i=0;i<120;i++)assert(col[i].wall==-1);
 view.yaw=0;view.far_z=3;assert(vcpe_raycast(&view,&wall,1,4,col,120)==120);
 assert(col[60].wall==-1);view.far_z=25;
 assert(vcpe_raycast(&view,&wall,1,4,col,119)==-1);
 uint32_t nanbits=0x7fc00000;memcpy(&view.x,&nanbits,4);
 assert(vcpe_raycast(&view,&wall,1,4,col,120)==-1);view.x=0;
 VcpeRayWall door[3]={{-3,-4,-1,-4,0,3,0,0xffffffff},{1,-4,3,-4,0,3,0,0xffffffff},{-20,-8,20,-8,0,3,0,0xffffffff}};
 assert(vcpe_raycast(&view,door,3,4,col,120)==120);
 assert(col[60].wall==2&&col[60].depth>7.999f&&col[60].depth<8.001f);
 assert(col[30].wall==0&&col[90].wall==1);
 puts("PASS wall strips: perpendicular depth, behind/far rejection, capacity, invalid floats, doorway visibility");
}
