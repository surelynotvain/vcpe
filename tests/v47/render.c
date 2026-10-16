#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../runtime/w_render.c"
World W;
const SprDef g_sprites[4]={{.ox=-8,.oy=-8,.w=16,.h=16},{.ox=-100,.oy=12,.w=80,.h=30}};
int main(void)
{
    Item expected[200];
    for(int frame=0;frame<600;frame++) {
        nitems=0;
        for(int i=0;i<200;i++) push(i%7,i%3,i%11,(float)(i%17),i%2,i%5,(float)(i%13),i);
        if(frame>=300) items[7].y1=(float)(frame%11);
        for(int i=0;i<200;i++) expected[i]=items[i];
        qsort(expected,200,sizeof *expected,cmp);
        int cached=prepare_order();
        for(int i=0;i<200;i++) {
            Item *got=&items[cached?order_cache[i]:i];
            assert(!cmp(got,&expected[i]) && got->idx==expected[i].idx && got->kind==expected[i].kind);
        }
    }
    assert(render_sort_hits>=299);
    assert(sprite_visible(0,1,0,0,1,240,136));
    assert(!sprite_visible(0,1,0,0,1,1000,136));
    for(int i=0;i<10000;i++) {
        float A=(rand()%100-50)*.05f,B=(rand()%100-50)*.05f,C=(rand()%100-50)*.05f,D=(rand()%100-50)*.05f;
        float x=(float)(rand()%1000-250),y=(float)(rand()%600-200);
        const SprDef *d=&g_sprites[i%2]; float loX=1e9f,hiX=-1e9f,loY=1e9f,hiY=-1e9f;
        for(int k=0;k<4;k++) {
            float px=d->ox+((k&1)?d->w:0),py=d->oy+((k&2)?d->h:0),sx=x+A*px+B*py,sy=y+C*px+D*py;
            if(sx<loX)loX=sx;
            if(sx>hiX)hiX=sx;
            if(sy<loY)loY=sy;
            if(sy>hiY)hiY=sy;
        }
        if(hiX>=0 && loX<=SCR_W && hiY>=0 && loY<=SCR_H) assert(sprite_visible(i%2,A,B,C,D,x,y));
    }
    printf("renderer: 600 exact ordering comparisons passed (%d cached sorts); 10000 conservative rotated/mirrored bounds passed\n",render_sort_hits);
    free(items); free(order_cache);
}
