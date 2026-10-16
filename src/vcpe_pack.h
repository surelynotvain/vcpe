/* VCPE compact loading: asset packs that load with one read and unload instantly.
 *
 * A pack is a block the converter writes (tools/vcpe_packer.py, "VPK1"): a small table of entries (name hash,
 * offset, size, kind) followed by the assets themselves, each 64-byte aligned, so the block can be used exactly as
 * it lands in memory - textures straight from it, no parsing, no per-asset allocation. A game puts one pack per
 * scene, per character, per sound bank... into its PAK.
 *
 * Packs load into slots of a pool (one memory block cut into equal slots, made once with vcpe_pack_pool). Loading
 * = one PAK read into a free slot; unloading = marking the slot free. Nothing is allocated or freed while
 * playing, so the heap can't fragment, and a load costs only the read itself.
 *
 * Automatic priority list: every pack has a priority (0..100). When no slot is free, the loader evicts the pack
 * with the lowest priority, and among those the one used longest ago (vcpe_pack_touch / vcpe_pack_find count as a
 * use). Pinned packs (vcpe_pack_pin) and packs still loading are never evicted. If nothing can go, the load fails.
 *
 * Background loading: vcpe_pack_load_async books a vcpe_jobs reservation (client "pack loader") that reads the pack
 * on a server thread with its own file handle while the game keeps running; the slot becomes usable when the
 * done callback runs on the main thread. Without vcpe_jobs linked it loads at once.
 *
 *   vcpe_pack_pool(0, 6 * 1024 * 1024, 3);                       // 3 slots of 2 MB
 *   int forest = vcpe_pack_load(PAK_FOREST_OFF, PAK_FOREST_SIZE, 60);
 *   const void *tex = vcpe_pack_find(forest, "tree.tex", &size);
 *   vcpe_pack_unload(forest);                                    // instant */
#ifndef VCPE_PACK_H
#define VCPE_PACK_H
#include <stdint.h>

#define VCPE_PACK_MAX_SLOTS 16

typedef struct { char magic[4]; uint32_t version, size, count, table; uint32_t pad[11]; } VcpePackHeader;   /* 64 B */
typedef struct { uint32_t hash, off, size; uint16_t kind, flags; } VcpePackEntry;   /* sorted by hash */

enum { VCPE_PACK_FREE, VCPE_PACK_LOADING, VCPE_PACK_READY };

/* the pool: mem 0 = allocate size bytes now (64-byte aligned); else memory the game owns (e.g. the volatile
 * partition: then reload packs after a sleep). Returns the slot size in bytes, -1 on failure */
int vcpe_pack_pool(void *mem, uint32_t size, int nslots);
void vcpe_pack_pool_free(void);

/* load the pack stored at PAK offset off (size bytes) with a priority 0..100. Returns a handle, -1 when it does not
 * fit a slot, no slot can be freed, or the read / header check fails. A pack already loaded from the same offset
 * returns its handle (its priority is raised if the new one is higher) */
int vcpe_pack_load(uint32_t off, uint32_t size, int priority);
typedef void (*VcpePackReady)(int handle, int ok, void *user);
int vcpe_pack_load_async(uint32_t off, uint32_t size, int priority, VcpePackReady ready, void *user);
/* Duplicate in-flight requests share one read and get callbacks only when READY (or failed). Up to 64 callbacks
 * may wait across all slots; -1 means the request was not accepted and no callback is owed. */
void vcpe_pack_unload(int handle);
/* Existing READY/LOADING handle without issuing I/O, or -1. */
int vcpe_pack_cached(uint32_t off,uint32_t size);
void vcpe_pack_unload_all(void);
int vcpe_pack_state(int handle);           /* VCPE_PACK_* (FREE also for a stale handle) */
void vcpe_pack_pin(int handle, int on);    /* pinned: never evicted */
void vcpe_pack_priority(int handle, int priority);
void vcpe_pack_touch(int handle);          /* counts as a use for the least-recently-used choice */

/* assets inside a ready pack, by name ("tree.tex", hashed with FNV-1a like the packer) or by hash */
const void *vcpe_pack_find(int handle, const char *name, uint32_t *size);
const void *vcpe_pack_get(int handle, uint32_t hash, uint32_t *size, int *kind);
int vcpe_pack_count(int handle);
const VcpePackEntry *vcpe_pack_entry(int handle, int i);
uint8_t *vcpe_pack_mem(int handle);        /* the whole pack (header first) */

/* the priority list as one line, highest first: "packs: @0x40000 60 pinned, @0x9c000 50, @0x120000 20 (1 free)"
 * (each pack shown by its PAK offset) */
int vcpe_pack_line(char *out, int n);
extern int vcpe_pack_loads, vcpe_pack_evictions;

/* used by the async loader to open its own handle on the PAK (set by vcpe_pack_pool from vcpe_pak; a game that
 * keeps its packs in another file sets it) */
void vcpe_pack_file(const char *path);

#endif
