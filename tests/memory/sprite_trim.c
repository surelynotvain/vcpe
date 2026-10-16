#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../src/vcpe_spr.c"
static int in_frame, waits, flushes;
int vcpe_gfx_in_frame(void) { return in_frame; }
void vcpe_gfx_flush(void) { flushes++; }
void vcpe_gfx_gpu_wait(void) { waits++; }
int main(void)
{
    uint8_t *mem[4]; uint32_t used[4] = {1,499,500,100};
    int32_t prev[4], next[4];
    VcpeSprPage tab[4] = {{.bpp=4},{.bpp=8},{.bpp=8},{.bpp=4}};
    hdr.npages=4; page_tab=tab; page_mem=mem; page_used=used; lru_prev=prev; lru_next=next;
    for (int i=0; i<4; i++) { mem[i]=malloc(page_bytes(i)); assert(mem[i]); resident+=page_bytes(i); }
    lru_front(0); lru_front(3); lru_front(1); lru_front(2);
    clock_=500; bound_page=0;
    in_frame=1; assert(!vcpe_spr_trim(128*1024,2,120) && !waits); in_frame=0;
    all_loaded=1; assert(!vcpe_spr_trim(128*1024,2,120)); all_loaded=0;
    arena=(uint8_t *)1; assert(!vcpe_spr_trim(128*1024,2,120)); arena=0;
    all_mem=(uint8_t *)1; assert(!vcpe_spr_trim(128*1024,2,120)); all_mem=0;
    assert(vcpe_spr_trim(128*1024,2,120)==64*1024);
    assert(!mem[0] && !mem[3] && mem[1] && mem[2] && bound_page==-1);
    assert(resident==128*1024 && waits==1 && flushes==1 && lru_tail==1 && lru_head==2);
    assert(!vcpe_spr_trim(65536,2,120) && waits==1); /* recent pages stay */
    assert(vcpe_spr_trim(65536,2,1)==65536 && !mem[1] && mem[2]);
    assert(!vcpe_spr_trim(65536,2,0)); /* current-frame page stays even at zero minimum age */
    clock_+=120;
    assert(vcpe_spr_trim(65536,1,120)==65536 && !resident && lru_head==-1 && lru_tail==-1);
    assert(waits==3 && flushes==3);
    puts("sprite trim checks passed: bounded cold-page LRU reclamation; current/recent pages and fixed pools preserved");
}
