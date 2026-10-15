#include "vcpe_tiles.h"
#include "vcpe_spr.h"
#include "vcpe_gfx.h"
#include <stdlib.h>
#include <math.h>
#define CAP 4096
#define CACHE_LAYERS 8
typedef struct {int image,l,t,w,h;float x,y;uint32_t color;unsigned serial;} Tile;
typedef struct {uint64_t keys[CAP];uint16_t order[CAP];unsigned count;int valid;} Order;
static Tile queue[CAP];static Order cache[CACHE_LAYERS];static unsigned count,layer;static uint32_t frame=~0u;static float camera_x,camera_y,sx,sy;static int reorder,cache_ok,slot;
unsigned vcpe_stat_tile_quads,vcpe_stat_tile_culled;
static uint64_t key(const Tile *t){return ((uint64_t)(uint32_t)t->image<<32)|((uint64_t)(uint16_t)(t->t>>8)<<16)|(uint16_t)(t->l>>8);}
static int compare(const void *aa,const void *bb){const Tile *a=aa,*b=bb;uint64_t ka=key(a),kb=key(b);if(ka!=kb)return ka<kb?-1:1;return (a->serial>b->serial)-(a->serial<b->serial);}
static void draw(const Tile *t){/* Snap shared boundaries identically. Fractional sprite edges can cover the same
 physical pixel in both neighboring tiles, making a harmless reorder visible. */float x0=floorf(t->x+.5f),y0=floorf(t->y+.5f),x1=floorf(t->x+t->w*sx+.5f),y1=floorf(t->y+t->h*sy+.5f);vcpe_spr_draw_part(t->image,0,t->l,t->t,t->w,t->h,x0,y0,(x1-x0)/t->w,(y1-y0)/t->h,t->color);}
static void flush(void){Order *o=slot>=0?&cache[slot]:0;if(reorder&&o&&cache_ok&&o->valid&&o->count==count){for(unsigned i=0;i<count;i++)draw(&queue[o->order[i]]);}else{if(reorder){qsort(queue,count,sizeof(*queue),compare);if(o){for(unsigned i=0;i<count;i++){o->order[i]=queue[i].serial;o->keys[queue[i].serial]=key(&queue[i]);}o->count=count;o->valid=1;}}for(unsigned i=0;i<count;i++)draw(&queue[i]);}count=0;}
void vcpe_tiles_begin(float x,float y,float xs,float ys,int disjoint){if(count)flush();if(frame!=vcpe_gfx_frame){frame=vcpe_gfx_frame;layer=0;}slot=layer<CACHE_LAYERS?(int)layer:-1;layer++;camera_x=x;camera_y=y;sx=xs;sy=ys;reorder=disjoint;cache_ok=1;vcpe_stat_tile_quads=vcpe_stat_tile_culled=0;}
void vcpe_tiles_part(int image,int l,int t,int w,int h,float x,float y,uint32_t color){x=(x-camera_x)*sx;y=(y-camera_y)*sy;if(image<0||w<=0||h<=0||!(color>>24)||x+w*sx<=0||y+h*sy<=0||x>=VCPE_SCR_W||y>=VCPE_SCR_H){vcpe_stat_tile_culled++;return;}if(count==CAP){flush();slot=-1;}queue[count]=(Tile){image,l,t,w,h,x,y,color,count};if(slot>=0&&cache[slot].valid&&cache[slot].keys[count]!=key(&queue[count]))cache_ok=0;count++;vcpe_stat_tile_quads++;}
void vcpe_tiles_end(void){flush();}
