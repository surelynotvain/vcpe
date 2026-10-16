#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
static int fail,allocations;
static void *test_malloc(size_t n){allocations++;return fail?0:malloc(n);}
#define malloc test_malloc
#include "../../src/vcpe_cull.c"
#undef malloc
int main(void)
{
    Vertex v[3]={0},w[3]={0};uint16_t indices[3]={0,1,2};
    vcpe_cull_limits(8192,128);fail=1;
    assert(vcpe_cull_prepare(v,3,indices,3)<0 && !vcpe_cull_find(v,indices,3) && !stats.bytes);
    fail=0;assert(!vcpe_cull_prepare(v,3,indices,3));uint32_t bytes=stats.bytes;
    fail=1;assert(vcpe_cull_prepare(w,3,indices,3)<0 && stats.bytes==bytes && vcpe_cull_find(v,indices,3));
    int calls=allocations;v[0].xyz[0]=20;vcpe_cull_dirty(v);
    assert(vcpe_cull_find(v,indices,3) && allocations==calls && stats.refits==1);
    vcpe_cull_reset();assert(!stats.bytes);
    puts("PASS culling allocation failures: uncached fallback, existing cache preserved, dirty refits allocate nothing");
}
