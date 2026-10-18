#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <pspgu.h>
#include "vcpe_perspective.h"
static int in_frame=1,calls,drawn,no_mem,missing,depth_disabled,opaque_enables;
static VcpeVtx arena[1024];static unsigned used;
int vcpe_stat_draws;
int vcpe_gfx_in_frame(void){return in_frame;}
void vcpe_gfx_flush(void){}
void vcpe_opaque(int on){assert(on==0||on==1);if(on)opaque_enables++;}
void vcpe_blend(int mode){assert(mode==0);}
void vcpe_filter(int nearest){assert(nearest==1);}
void vcpe_tex_white(int on){assert(on==0);}
void vcpe_unbind(void){}
int vcpe_bind_page(int page){return page!=missing;}
void vcpe_page_size(int page,int *w,int *h){(void)page;*w=32;*h=64;}
void *vcpe_gfx_mem(int bytes){if(no_mem)return 0;assert(used+bytes<=sizeof arena);void *p=(char *)arena+used;used+=bytes;return p;}
void sceGuDisable(int feature){if(feature==GU_DEPTH_TEST)depth_disabled++;}
void sceGuEnable(int feature){assert(feature!=GU_DEPTH_TEST);}
void sceGuAlphaFunc(int a,int b,int c){(void)a;(void)b;(void)c;}
void sceGuTexWrap(int a,int b){assert(a==GU_CLAMP&&b==GU_CLAMP);}
void sceGuDrawArray(int mode,int flags,int n,const void *index,const void *vertices){
 assert(mode==GU_TRIANGLES&&flags==(GU_TEXTURE_32BITF|GU_COLOR_8888|GU_VERTEX_32BITF|GU_TRANSFORM_2D));
 assert(!index&&n>0&&n<=192);const VcpeVtx *v=vertices;
 for(int j=0;j<n;j++)assert(v[j].u==16&&v[j].v==32&&v[j].z==0);
 calls++;drawn+=n/3;
}
int main(void){
 VcpePerspectiveTriangle tri[70]={0};
 for(int i=0;i<70;i++){
  tri[i].depth=(float)i;tri[i].order=(unsigned)i;tri[i].page=0;
  for(int j=0;j<3;j++){tri[i].v[j].u=.5f;tri[i].v[j].v=.5f;}
 }
 VcpePerspective p={0};p.valid=1;p.triangles=tri;p.count=p.capacity=70;missing=-99;
 assert(vcpe_perspective_draw(&p)==70&&p.submitted==70&&p.dropped==0);
 assert(calls==2&&drawn==70&&vcpe_stat_draws==2&&depth_disabled==1);
 assert(tri[0].v[0].u==.5f); /* CPU queue is not changed into texture pixels */
 in_frame=0;assert(vcpe_perspective_draw(&p)==0&&calls==2);in_frame=1;
 no_mem=1;p.dropped=0;assert(!vcpe_perspective_draw(&p)&&p.dropped==70);
 no_mem=0;missing=0;p.dropped=0;assert(!vcpe_perspective_draw(&p)&&p.dropped==70);
 missing=-99;used=0;p.dropped=0;
 assert(vcpe_perspective_draw_ex(&p,VCPE_PERSPECTIVE_OPAQUE)==70&&opaque_enables==1);
 puts("PASS 2D-only GU submission, bounded batches, pixel UVs, no depth enable, frame guard, texture/allocation failure");
}
