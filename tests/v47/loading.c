#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/vcpe_pack.c"
#include "vcpe_prefetch.h"
static struct { VcpeJobFn fn; VcpeJobDone done; void *arg; } pending[64];
static int workers=1,available=8192,callbacks;
int vcpe_servers_count(void) { return workers; }
int vcpe_mem_available_kb(void) { return available; }
int vcpe_client_add(const char *n,int p) { (void)n;(void)p;return 0; }
int vcpe_reserve(int c,VcpeJobFn fn,void *arg,VcpeJobDone done,int u)
{
    (void)c;(void)u;
    for(int i=0;i<64;i++) if(!pending[i].fn) { pending[i].fn=fn;pending[i].arg=arg;pending[i].done=done;return i; }
    return -1;
}
void vcpe_jobs_wait_all(void)
{
    for(int i=0;i<64;i++) if(pending[i].fn) {
        VcpeJobFn fn=pending[i].fn; VcpeJobDone done=pending[i].done; void *arg=pending[i].arg;
        pending[i].fn=0; fn(arg); if(done)done(i,arg);
    }
}
const char *vcpe_pak_path(void) { return "fixture.pak"; }
int vcpe_pak_read(uint32_t off,void *dst,uint32_t n)
{
    FILE *f=fopen("fixture.pak","rb");assert(f);int ok=!fseek(f,off,SEEK_SET)&&fread(dst,1,n,f)==n;fclose(f);return ok?0:-1;
}
static void ready(int h,int ok,void *user)
{
    assert(ok && h>=0 && vcpe_pack_mem(h)); callbacks++; if(user)(*(int *)user)++;
}
int main(void)
{
    uint8_t bytes[576]={0};
    for(int i=0;i<3;i++) {
        VcpePackHeader *h=(void *)(bytes+i*192); memcpy(h->magic,"VPK1",4);h->version=1;h->size=192;h->table=64;h->count=1;
        VcpePackEntry *e=(void *)(bytes+i*192+64);e->hash=fnv("name");e->off=128;e->size=3;
        memcpy(bytes+i*192+128,i?"xyz":"abc",3);
    }
    ((VcpePackHeader *)(void *)(bytes+384))->table=UINT32_MAX-3;
    FILE *f=fopen("fixture.pak","wb");assert(f);assert(fwrite(bytes,1,sizeof bytes,f)==sizeof bytes);fclose(f);
    assert(vcpe_pack_pool(0,768,3)==256); vcpe_pack_file("fixture.pak");
    int a=0,b=0,h=vcpe_pack_load_async(0,192,50,ready,&a); assert(h>=0);
    assert(vcpe_pack_load_async(0,192,50,ready,&b)==h && callbacks==0);
    assert(vcpe_pack_load_async(0,191,50,ready,0)==-1 && callbacks==0);
    vcpe_jobs_wait_all();assert(a==1&&b==1&&callbacks==2&&vcpe_pack_state(h)==VCPE_PACK_READY);
    uint32_t size;assert(!memcmp(vcpe_pack_find(h,"name",&size),"abc",3)&&size==3);
    assert(vcpe_pack_load(384,192,0)==-1); /* header arithmetic cannot wrap into an accepted table */
    vcpe_pack_pool_free(); assert(vcpe_pack_pool(0,768,3)==256);
    h=vcpe_pack_load_async(0,192,50,ready,0);
    for(int i=1;i<64;i++)assert(vcpe_pack_load_async(0,192,50,ready,0)==h);
    assert(vcpe_pack_load_async(0,192,50,ready,0)==-1);
    vcpe_jobs_wait_all();assert(callbacks==66);
    vcpe_pack_pool_free(); assert(vcpe_pack_pool(0,768,3)==256);
    slots[0].gen=0x7fffff; assert(vcpe_pack_load(0,192,1)>=0); /* generation sign bit stays clear */
    vcpe_pack_pool_free(); assert(vcpe_pack_pool(0,768,3)==256);
    VcpePrefetchAsset assets[2]={{10,1,0,192,70},{20,2,192,192,30}};
    assert(vcpe_prefetch_init(assets,2,384,1024)==0);
    vcpe_prefetch_hint(1);workers=0;assert(vcpe_prefetch_update(0)==0);
    assert(vcpe_prefetch_update(1)==1&&vcpe_prefetch_take(10)==-1);
    vcpe_jobs_wait_all();assert(vcpe_prefetch_stats()->owned_bytes==192);
    h=vcpe_prefetch_take(10);assert(h>=0&&slots[h&255].pinned&&vcpe_prefetch_stats()->owned_bytes==0);
    workers=1;vcpe_prefetch_hint(2);assert(vcpe_prefetch_update(0)==1);
    vcpe_prefetch_hint(3);vcpe_jobs_wait_all();assert(vcpe_prefetch_stats()->cancelled==1);
    assert(slots[h&255].pinned&&vcpe_pack_state(h)==VCPE_PACK_READY); /* never unpin somebody else's active asset */
    available=1;vcpe_prefetch_hint(2);assert(vcpe_prefetch_update(0)==0);available=8192;
    vcpe_prefetch_scene(0);vcpe_prefetch_scene(1);vcpe_prefetch_scene(0);
    assert(vcpe_prefetch_stats()->predicted_scene==1);
    vcpe_prefetch_shutdown();vcpe_pack_unload(h);vcpe_pack_pool_free();
    remove("fixture.pak");
    puts("loading: duplicate async readiness, 64 waiters, overflow/generation checks, predictive dependencies, pressure, cancellation and ownership passed");
}
