#include <string.h>
#include "vcpe_prefetch.h"
#include "vcpe_pack.h"
#include "vcpe_jobs.h"
#include "vcpe_adapt.h"
int vcpe_adapt_work_add(VcpeAdaptWork,void *) __attribute__((weak));
void vcpe_adapt_work_remove(int) __attribute__((weak));
int vcpe_servers_count(void) __attribute__((weak));
void vcpe_jobs_wait_all(void) __attribute__((weak));
int vcpe_mem_available_kb(void) __attribute__((weak));
static const VcpePrefetchAsset *table;
static int count,current=-1,task=-1,reserve,nedges;
static uint32_t limit;
enum { IDLE,LOADING,READY,TAKEN,FAILED };
static struct { int state,wanted,owned,handle; } entries[64];
static struct { int from,to; uint32_t visits; } edges[64];
static VcpePrefetchStats st={.predicted_scene=-1};
static void discard(int i)
{
    if (entries[i].owned && entries[i].state==READY) {
        vcpe_pack_pin(entries[i].handle,0); vcpe_pack_unload(entries[i].handle);
        st.owned_bytes-=table[i].size; entries[i].owned=0; entries[i].state=IDLE; st.cancelled++;
    }
}
static void loaded(int h,int ok,void *user)
{
    int i=(int)(uintptr_t)user-1;
    entries[i].handle=h; entries[i].state=ok?READY:FAILED; st.pending--;
    if (!ok) {
        if (entries[i].owned) st.owned_bytes-=table[i].size;
        entries[i].owned=0; st.failures++; return;
    }
    if (entries[i].owned) vcpe_pack_pin(h,1);
    if (!entries[i].wanted) discard(i);
}
static uint32_t maintenance(int phase,uint32_t budget,void *user)
{
    (void)user;
    if (budget && phase!=VCPE_ADAPT_PRESSURE) vcpe_prefetch_update(phase>=VCPE_ADAPT_LOAD_BEGIN);
    return 0;
}
void vcpe_prefetch_shutdown(void)
{
    if (task>=0 && vcpe_adapt_work_remove) vcpe_adapt_work_remove(task);
    task=-1;
    for (int i=0;i<count;i++) { entries[i].wanted=0; discard(i); }
    if (vcpe_jobs_wait_all) vcpe_jobs_wait_all();
    table=0; count=0; current=-1; nedges=0; memset(entries,0,sizeof entries);
}
int vcpe_prefetch_init(const VcpePrefetchAsset *assets,int n,uint32_t budget,int reserve_kb)
{
    if (!assets || n<1 || n>64 || !budget || reserve_kb<0) return -1;
    for (int i=0;i<n;i++) {
        if (assets[i].id<0 || assets[i].scene<0 || assets[i].size<sizeof(VcpePackHeader) ||
            assets[i].size>UINT32_MAX-assets[i].off) return -1;
        for (int j=0;j<i;j++) if (assets[j].id==assets[i].id || assets[j].off==assets[i].off) return -1;
    }
    vcpe_prefetch_shutdown(); table=assets; count=n; limit=budget; reserve=reserve_kb;
    memset(&st,0,sizeof st); st.predicted_scene=-1;
    if (vcpe_adapt_work_add) task=vcpe_adapt_work_add(maintenance,0);
    return 0;
}
void vcpe_prefetch_hint(int scene)
{
    st.predicted_scene=scene;
    for (int i=0;i<count;i++) {
        entries[i].wanted=table[i].scene==scene || table[i].scene==current;
        if (!entries[i].wanted) discard(i);
        else if (entries[i].state==FAILED) entries[i].state=IDLE;
    }
}
void vcpe_prefetch_scene(int scene)
{
    if (scene<0) return;
    if (current>=0 && scene!=current) {
        int i; for (i=0;i<nedges;i++) if (edges[i].from==current && edges[i].to==scene) break;
        if (i==nedges) {
            if (nedges<64) nedges++;
            else { i=0; for (int j=1;j<64;j++) if (edges[j].visits<edges[i].visits) i=j; }
            edges[i].from=current; edges[i].to=scene; edges[i].visits=0;
        }
        if (edges[i].visits<UINT32_MAX) edges[i].visits++;
    }
    current=scene;
    int best=-1;
    for (int i=0;i<nedges;i++) if (edges[i].from==scene && (best<0 || edges[i].visits>edges[best].visits)) best=i;
    vcpe_prefetch_hint(best>=0?edges[best].to:-1);
}
int vcpe_prefetch_update(int loading)
{
    if (!table || (!loading && (!vcpe_servers_count || vcpe_servers_count()<1)) || st.pending>=2) return 0;
    int available=vcpe_mem_available_kb?vcpe_mem_available_kb():-1;
    if (available>=0 && available<reserve) return 0;
    int best=-1;
    for (int i=0;i<count;i++) {
        if ((entries[i].state==READY || entries[i].state==TAKEN) && vcpe_pack_state(entries[i].handle)!=VCPE_PACK_READY) {
            if (entries[i].owned) st.owned_bytes-=table[i].size;
            entries[i].owned=0; entries[i].state=IDLE;
        }
        if (entries[i].wanted && entries[i].state==IDLE && table[i].size<=limit-st.owned_bytes &&
            (best<0 || table[i].priority>table[best].priority)) best=i;
    }
    if (best<0) return 0;
    int i=best,cached=vcpe_pack_cached(table[i].off,table[i].size);
    entries[i].owned=cached<0;
    if (entries[i].owned) st.owned_bytes+=table[i].size;
    entries[i].state=LOADING; st.pending++;
    int h=vcpe_pack_load_async(table[i].off,table[i].size,table[i].priority,loaded,(void *)(uintptr_t)(i+1));
    if (h<0) { if (entries[i].state==LOADING) loaded(-1,0,(void *)(uintptr_t)(i+1)); return -1; }
    entries[i].handle=h; st.submitted++; return 1;
}
int vcpe_prefetch_take(int id)
{
    for (int i=0;i<count;i++) if (table[i].id==id) {
        if ((entries[i].state==READY || entries[i].state==TAKEN) && vcpe_pack_state(entries[i].handle)==VCPE_PACK_READY) {
            if (entries[i].owned) st.owned_bytes-=table[i].size;
            entries[i].owned=0; entries[i].state=TAKEN; st.hits++; return entries[i].handle;
        }
        st.misses++; return -1;
    }
    st.misses++; return -1;
}
const VcpePrefetchStats *vcpe_prefetch_stats(void) { return &st; }
