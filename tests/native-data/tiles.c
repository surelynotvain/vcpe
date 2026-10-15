#include "vcpe_tiles.h"
#include <assert.h>
#include <stdio.h>
uint32_t vcpe_gfx_frame;
static int ids[10],n;static float pos[10];
void vcpe_spr_draw_part(int img,int frame,int l,int t,int w,int h,float x,float y,float xs,float ys,unsigned color){ids[n]=img;pos[n++]=x;}
int main(void){vcpe_tiles_begin(10,0,.5f,.5f,0);vcpe_tiles_part(2,0,0,32,32,30,0,~0u);vcpe_tiles_part(1,0,0,32,32,70,0,~0u);vcpe_tiles_part(3,0,0,32,32,2000,0,~0u);vcpe_tiles_end();assert(n==2&&ids[0]==2&&ids[1]==1&&pos[0]==10&&pos[1]==30);assert(vcpe_stat_tile_culled==1);n=0;vcpe_tiles_begin(0,0,1,1,1);vcpe_tiles_part(2,0,0,32,32,0,0,~0u);vcpe_tiles_part(1,0,0,32,32,32,0,~0u);vcpe_tiles_end();assert(n==2&&ids[0]==1&&ids[1]==2);puts("Tile culling, scale, ordered overlap and opt-in grouping passed");}
