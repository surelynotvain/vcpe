#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
static int fail_page, page_allocations;
static void *font_memalign(size_t a,size_t n)
{
    page_allocations++;
    if(fail_page){fail_page=0;return 0;}
    void *p=0;return posix_memalign(&p,a,n)?0:p;
}
#define memalign font_memalign
#ifndef TTF_SOURCE
#define TTF_SOURCE "../../src/vcpe_ttf.c"
#endif
#include TTF_SOURCE
#undef memalign
uint32_t vcpe_gfx_frame=1;
static const void *bound_key;
static const uint8_t *texture;
static uint64_t digest=1469598103934665603ull;
static void hash(const void *p,size_t n){const uint8_t *b=p;while(n--){digest^=*b++;digest*=1099511628211ull;}}
void sceKernelDcacheWritebackRange(const void *p,unsigned n){(void)p;(void)n;}
void sceKernelDcacheWritebackAll(void){}
void vcpe_gfx_gpu_wait(void){} void vcpe_gfx_flush(void){}
void vcpe_tex_uploaded(void){} void vcpe_tex_flush(void){}
void vcpe_blend(int n){(void)n;} void vcpe_filter(int n){(void)n;} void vcpe_tex_white(int n){(void)n;}
int vcpe_tex_bound(const void *p){return bound_key==p;} void vcpe_tex_claim(const void *p){bound_key=p;}
void sceGuClutMode(int a,int b,int c,int d){(void)a;(void)b;(void)c;(void)d;}
void sceGuClutLoad(int a,const void *p){(void)a;(void)p;}
void sceGuTexMode(int a,int b,int c,int d){(void)a;(void)b;(void)c;(void)d;}
void sceGuTexImage(int a,int b,int c,int d,const void *p){(void)a;(void)b;(void)c;(void)d;texture=p;}
void vcpe_quad(float x0,float y0,float x1,float y1,float u0,float v0,float u1,float v1,uint32_t color)
{
    hash(&x0,4);hash(&y0,4);hash(&x1,4);hash(&y1,4);hash(&color,4);
    for(int y=(int)v0;y<(int)v1;y++)hash(texture+y*PAGE_W+(int)u0,(size_t)(u1-u0));
}
int main(int argc,char **argv)
{
    assert(argc>1);FILE *file=fopen(argv[1],"rb");assert(file);fseek(file,0,SEEK_END);long n=ftell(file);rewind(file);
    void *font=malloc((size_t)n);assert(font&&fread(font,1,(size_t)n,file)==(size_t)n);fclose(file);
    VcpeTtf *f=vcpe_ttf_open(font,(uint32_t)n);assert(f);
#ifdef FONT_NEW
    fail_page=1;assert(!init()&&!pages[0].tex&&!pages[1].tex);
    assert(init()&&pages[0].tex&&!pages[1].tex&&vcpe_ttf_resident_bytes()==262144);
#endif
    for(int frame=0;frame<30;frame++){
        vcpe_gfx_frame++;float advance=vcpe_ttf_draw(f,13,8.25f,24,"AVATAR fi abc 0123 Türkçe",0xffc0ffff);
        assert(advance>0);hash(&advance,sizeof advance);
    }
    printf("font output hash %llx\n",(unsigned long long)digest);
#ifdef FONT_NEW
    assert(!pages[1].tex);
    pages[0].x=0;pages[0].y=PAGE_H;pages[0].row=0;vcpe_gfx_frame++;
    int u,v;fail_page=1;assert(alloc(32,32,&u,&v)==-1&&pages[0].tex&&!pages[1].tex);
    assert(alloc(32,32,&u,&v)==1&&vcpe_ttf_resident_bytes()==524288);
    /* Full tables and wrapping collision chains retain every survivor after either page is removed. */
    for(int pass=0;pass<20;pass++){
        memset(slots,0,sizeof slots);nslots=0;
        uint32_t keys[GLYPH_SLOTS];
        for(int i=0;i<GLYPH_SLOTS;i++){
            keys[i]=(uint32_t)(i+1)*2654435761u+(uint32_t)pass;
            if(!keys[i])keys[i]=1;
            Slot *s=slot_find(keys[i],(uint16_t)i);assert(s&&!s->key);
            *s=(Slot){.key=keys[i],.gl=(uint16_t)i,.page=(uint8_t)(i&1)};nslots++;
        }
        slots_drop_page(pass&1);assert(nslots==GLYPH_SLOTS/2);
        for(int i=0;i<GLYPH_SLOTS;i++)if((i&1)!=(pass&1)){
            Slot *s=slot_find(keys[i],(uint16_t)i);assert(s&&s->key==keys[i]&&s->gl==i);
        }
        slots_drop_page(!(pass&1));assert(!nslots);
    }
    vcpe_ttf_cache_reset();
    for(int frame=0;frame<30;frame++){
        vcpe_gfx_frame++;
        for(int size=8;size<63;size++)vcpe_ttf_prewarm(f,(float)size,"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");
        vcpe_ttf_draw(f,14,5,24,"eviction and retry",0xffffffff);
    }
    printf("font: removed %zu-byte rebuild scratch; lazy 256 KiB page, failed allocation retry and 20 full-table rehash stress passes succeeded\n",sizeof(Slot)*GLYPH_SLOTS);
#endif
    for(int i=0;i<NPAGES;i++)free(pages[i].tex);
    (void)page_allocations;
    free(f);free(font);
}
