#include "vcpe_skin.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
static void near(float a,float b){assert(fabsf(a-b)<.0001f);}
int main(void){
 float k[2][2][12]={{{1,0,0,0,0,1,0,0,0,0,1,0},{1,0,0,2,0,1,0,0,0,0,1,0}},{{1,0,0,4,0,1,0,0,0,0,1,0},{1,0,0,6,0,1,0,0,0,0,1,0}}};
 VcpeSkinClip clip={k,2,2,2,0};float out[VCPE_SKIN_MAX_BONES][12];
 assert(!vcpe_skin_pose(&clip,1,1,out));near(out[0][3],2);near(out[1][3],4);
 VcpeSkinVertex v={.xyz={1,2,3},.bone={0,1},.weight={.25f,.75f}};VcpeVtx result={.u=.25f,.v=.75f,.color=0xdeadbeef};
 assert(!vcpe_skin_apply(&result,&v,1,out,2));near(result.x,4.5f);near(result.y,2);near(result.z,3);near(result.u,.25f);assert(result.color==0xdeadbeef);
 assert(!vcpe_skin_pose(&clip,2.5f,1,out));near(out[0][3],1);
 assert(!vcpe_skin_pose(&clip,3,0,out));near(out[0][3],4);
 assert(!vcpe_skin_pose(&clip,-1,0,out));near(out[0][3],0);
 uint16_t half[24]={0x3c00,0,0,0,0,0x3c00,0,0,0,0,0x3c00,0,0x3c00,0,0,0x4000,0,0x3c00,0,0,0,0,0x3c00,0};
 VcpeSkinClip h={half,2,1,1,1};assert(!vcpe_skin_pose(&h,.5f,0,out));near(out[0][3],1);
 half[3]=0x8000;half[15]=0xbc00;assert(!vcpe_skin_pose(&h,1,0,out));near(out[0][3],-1);
 v.bone[0]=2;assert(vcpe_skin_apply(&result,&v,1,out,2)<0);
 float aim[2][12]={{1,0,0,0,0,1,0,0,0,0,1,0},{1,0,0,7,0,1,0,0,0,0,1,0}};
 float bind[12]={1,0,0,0,0,1,0,0,0,0,1,0},target[3]={1,0,0};unsigned char mask[2]={1,0};
 assert(!vcpe_skin_aim(aim,2,0,mask,bind,target,1.570796327f));near(-aim[0][2],1);near(-aim[0][10],0);near(aim[1][3],7);
 near(aim[0][3],0);near(aim[0][7],0);near(aim[0][11],0);
 clip.bones=VCPE_SKIN_MAX_BONES+1;assert(vcpe_skin_pose(&clip,0,1,out)<0);
 puts("PASS weighted skinning, UV/color preservation, source timing, loop/clamp, half matrices and invalid bone/rig bounds");
}
