#ifndef VCPE_PREFETCH_H
#define VCPE_PREFETCH_H
#include <stdint.h>
typedef struct { int id,scene; uint32_t off,size; int priority; } VcpePrefetchAsset;
typedef struct { uint32_t submitted,hits,misses,failures,cancelled,owned_bytes; int predicted_scene,pending; } VcpePrefetchStats;
/* Table stays owned by caller. Maximum 64 unique IDs/offsets, nonnegative scenes. Configure the pack pool first.
 * budget bounds speculative payload, not physical pool allocation. Pending reads finish before shutdown. */
int vcpe_prefetch_init(const VcpePrefetchAsset *assets,int count,uint32_t budget,int reserve_kb);
void vcpe_prefetch_shutdown(void);
void vcpe_prefetch_scene(int scene);
void vcpe_prefetch_hint(int next_scene);
int vcpe_prefetch_update(int loading); /* one request; play requires actual worker servers */
int vcpe_prefetch_take(int id); /* READY handle or -1; speculative pin transfers to caller: unpin/unload when done */
const VcpePrefetchStats *vcpe_prefetch_stats(void);
#endif
