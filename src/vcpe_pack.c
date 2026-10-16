/* VCPE compact loading (see vcpe_pack.h).
 *
 * HOW THIS FILE WORKS
 * The pool is one block cut into nslots equal slots. Each slot remembers which pack it holds (its PAK offset),
 * a generation number (handles carry it, so a handle to an evicted pack is recognised as stale), its priority, its
 * last use (a counter that goes up on every use) and whether it is pinned.
 *
 * Choosing a slot for a new pack: a free slot if there is one; otherwise the READY, unpinned slot with the lowest
 * priority, and among equal priorities the one used longest ago. That is the "automatic priority list".
 *
 * Loading in the background: the slot is marked LOADING (not usable, not evictable), and a vcpe_jobs reservation
 * reads the pack on a server thread with its own FILE handle (the engine's PAK handle belongs to the main thread).
 * The done callback runs on the main thread: it checks the header and marks the slot READY, or frees it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include "vcpe_pak.h"
#include "vcpe_jobs.h"
#include "vcpe_pack.h"
#if defined(__psp__)
#include <pspkernel.h>
#endif

typedef struct {
    int state, gen, priority, pinned, ok;
    uint32_t off, size, used;
} Slot;

static uint8_t *pool, *pool_own;
static uint32_t slot_size;
static int nslots;
static Slot slots[VCPE_PACK_MAX_SLOTS];
#define MAX_WAITERS 64
static struct { int used, handle; VcpePackReady fn; void *user; } waiters[MAX_WAITERS];
static uint32_t use_clock;
static char file_path[300];
static int loader_client = -1;
int vcpe_pack_loads, vcpe_pack_evictions;

/* weak: vcpe_jobs is optional (without it, async loads happen at once) */
int vcpe_reserve(int client, VcpeJobFn fn, void *arg, VcpeJobDone done, int urgency) __attribute__((weak));
int vcpe_client_add(const char *name, int priority) __attribute__((weak));
void vcpe_jobs_wait_all(void) __attribute__((weak));
const char *vcpe_pak_path(void) __attribute__((weak));

static uint8_t *slot_mem(int s) { return pool + (uint32_t)s * slot_size; }
static int handle(int s) { return (slots[s].gen << 8) | s; }
static Slot *slot_of(int h, int *s_out)
{
    int s = h & 0xFF;
    if (h < 0 || s >= nslots || slots[s].gen != (h >> 8) || slots[s].state == VCPE_PACK_FREE)
        return 0;
    if (s_out)
        *s_out = s;
    return &slots[s];
}

static uint32_t fnv(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 16777619u;
    }
    return h;
}

/* the CPU wrote the pack through its data cache; the GE reads RAM, so write it back before textures are used */
static void writeback(void *p, uint32_t n)
{
#if defined(__psp__)
    sceKernelDcacheWritebackRange(p, n);
#else
    (void)p; (void)n;
#endif
}

/* a pack that landed in memory is only trusted if its header and table agree with the read */
static int header_ok(const uint8_t *m, uint32_t size)
{
    const VcpePackHeader *h = (const VcpePackHeader *)m;
    return size >= sizeof *h && !memcmp(h->magic, "VPK1", 4) && h->version == 1 && h->size == size &&
           h->table >= sizeof *h && h->table <= size && !(h->table & 3) && h->count < 65536 &&
           h->count <= (size - h->table) / sizeof(VcpePackEntry);
}

int vcpe_pack_pool(void *mem, uint32_t size, int n)
{
    if (n < 1 || n > VCPE_PACK_MAX_SLOTS || size < (uint32_t)n * 64 ||
        (mem && ((uintptr_t)mem & 63)) || (mem && mem == pool && pool_own))
        return -1;
    uint32_t each = (size / n) & ~63u;   /* slots start on 64-byte boundaries */
    uint8_t *own = 0;
    if (!mem && !(mem = own = memalign(64, each * n)))
        return -1;
    vcpe_pack_pool_free();
    pool_own = own;
    pool = mem;
    slot_size = each;
    nslots = n;
    for (int s = 0; s < n; s++)
        slots[s].state = VCPE_PACK_FREE;
    if (!file_path[0] && vcpe_pak_path)
        snprintf(file_path, sizeof file_path, "%s", vcpe_pak_path());
    return (int)each;
}

void vcpe_pack_pool_free(void)
{
    if (vcpe_jobs_wait_all)
        vcpe_jobs_wait_all();   /* no background read may still be writing into the pool */
    free(pool_own);
    pool = pool_own = 0;
    nslots = 0;
}

void vcpe_pack_file(const char *path)
{
    const char *next=path?path:"";
    if (!strcmp(file_path,next)) return;
    if (vcpe_jobs_wait_all) vcpe_jobs_wait_all();
    vcpe_pack_unload_all();
    snprintf(file_path,sizeof file_path,"%s",next);
}

/* a slot for a new pack: free first, else evict by the priority list. -1 when every slot is pinned or loading */
static int take_slot(void)
{
    int best = -1;
    for (int s = 0; s < nslots; s++) {
        if (slots[s].state == VCPE_PACK_FREE)
            return s;
        if (slots[s].state != VCPE_PACK_READY || slots[s].pinned)
            continue;
        if (best < 0 || slots[s].priority < slots[best].priority ||
            (slots[s].priority == slots[best].priority && slots[s].used < slots[best].used))
            best = s;
    }
    if (best >= 0) {
        vcpe_pack_evictions++;
        slots[best].state = VCPE_PACK_FREE;
    }
    return best;
}

/* the same pack already in memory (or on its way): reuse it */
static int find_loaded(uint32_t off, uint32_t size, int priority)
{
    priority=priority<0?0:priority>100?100:priority;
    for (int s = 0; s < nslots; s++)
        if (slots[s].state != VCPE_PACK_FREE && slots[s].off == off) {
            if (slots[s].size != size) return -2;
            if (priority > slots[s].priority)
                slots[s].priority = priority;
            slots[s].used = ++use_clock;
            return handle(s);
        }
    return -1;
}

int vcpe_pack_cached(uint32_t off,uint32_t size)
{
    int h=find_loaded(off,size,0); return h>=0?h:-1;
}

static int claim(uint32_t off, uint32_t size, int priority, int state)
{
    if (!pool || size > slot_size || size < sizeof(VcpePackHeader))
        return -1;
    int s = take_slot();
    if (s < 0)
        return -1;
    Slot *sl = &slots[s];
    int gen = sl->gen;
    memset(sl, 0, sizeof *sl);
    sl->gen = (gen + 1) & 0x7FFFFF;   /* positive signed handles, including after generation wrap */
    sl->state = state;
    sl->off = off;
    sl->size = size;
    sl->priority = priority < 0 ? 0 : priority > 100 ? 100 : priority;
    sl->used = ++use_clock;
    return s;
}

int vcpe_pack_load(uint32_t off, uint32_t size, int priority)
{
    int h = find_loaded(off, size, priority);
    if (h == -2) return -1;
    if (h >= 0)
        return h;
    int s = claim(off, size, priority, VCPE_PACK_LOADING);
    if (s < 0)
        return -1;
    if (vcpe_pak_read(off, slot_mem(s), size) != 0 || !header_ok(slot_mem(s), size)) {
        slots[s].state = VCPE_PACK_FREE;
        return -1;
    }
    writeback(slot_mem(s), size);
    slots[s].state = VCPE_PACK_READY;
    vcpe_pack_loads++;
    return handle(s);
}

/* ------------------------------------------------------------------ background loading */
typedef struct { int slot; uint32_t off, size; int ok; } Job;
static Job jobs[VCPE_PACK_MAX_SLOTS];

/* runs on a server thread: its own FILE handle, so it never disturbs the main thread's PAK reads */
static void read_job(void *arg)
{
    Job *j = arg;
    j->ok = 0;
    for (int attempt = 0; attempt < 3 && !j->ok; attempt++) {   /* the memory stick may be waking up */
        FILE *f = fopen(file_path, "rb");
        if (!f)
            continue;
        j->ok = fseek(f, (long)j->off, SEEK_SET) == 0 && fread(slot_mem(j->slot), 1, j->size, f) == j->size;
        fclose(f);
    }
}

/* main thread: the read finished */
static void read_done(int reservation, void *arg)
{
    (void)reservation;
    Job *j = arg;
    Slot *sl = &slots[j->slot];
    int ok = j->ok && header_ok(slot_mem(j->slot), j->size);
    if (ok) {
        writeback(slot_mem(j->slot), j->size);
        sl->state = VCPE_PACK_READY;
        vcpe_pack_loads++;
    } else {
        sl->state = VCPE_PACK_FREE;
    }
    int h = handle(j->slot), count = 0;
    VcpePackReady callbacks[MAX_WAITERS];
    void *users[MAX_WAITERS];
    /* Detach before invoking user code: callbacks can request or unload packs without corrupting this delivery. */
    for (int i=0; i<MAX_WAITERS; i++) if (waiters[i].used && waiters[i].handle == h) {
        callbacks[count] = waiters[i].fn; users[count++] = waiters[i].user; waiters[i].used = 0;
    }
    for (int i=0; i<count; i++) {
        int live = ok && vcpe_pack_state(h) == VCPE_PACK_READY;
        callbacks[i](live ? h : -1, live, users[i]);
    }
}

static int subscribe(int h, VcpePackReady fn, void *user)
{
    if (!fn) return 0;
    for (int i=0; i<MAX_WAITERS; i++) if (!waiters[i].used) {
        waiters[i].used=1; waiters[i].handle=h; waiters[i].fn=fn; waiters[i].user=user; return 0;
    }
    return -1;
}

int vcpe_pack_load_async(uint32_t off, uint32_t size, int priority, VcpePackReady ready, void *user)
{
    int h = find_loaded(off, size, priority);
    if (h == -2) return -1;
    if (h >= 0 && vcpe_pack_state(h) == VCPE_PACK_LOADING)
        return subscribe(h, ready, user) == 0 ? h : -1;
    if (h >= 0 || !vcpe_reserve || !file_path[0]) {
        if (h < 0)
            h = vcpe_pack_load(off, size, priority);   /* no jobs module: load now */
        if (ready)
            ready(h, h >= 0, user);
        return h;
    }
    int s = claim(off, size, priority, VCPE_PACK_LOADING);
    if (s < 0)
        return -1;
    if (subscribe(handle(s), ready, user) < 0) { slots[s].state = VCPE_PACK_FREE; return -1; }
    if (loader_client < 0)
        loader_client = vcpe_client_add("pack loader", 40);
    jobs[s] = (Job){s, off, size, 0};
    if (vcpe_reserve(loader_client, read_job, &jobs[s], read_done, priority / 10) < 0) {
        for (int i=0; i<MAX_WAITERS; i++) if (waiters[i].used && waiters[i].handle==handle(s)) waiters[i].used=0;
        slots[s].state = VCPE_PACK_FREE;
        return -1;
    }
    return handle(s);
}

/* ------------------------------------------------------------------ state, lookups */
void vcpe_pack_unload(int h)
{
    Slot *sl = slot_of(h, 0);
    if (sl && sl->state == VCPE_PACK_READY)   /* a pack still loading is freed by its done callback */
        sl->state = VCPE_PACK_FREE;
}

void vcpe_pack_unload_all(void)
{
    for (int s = 0; s < nslots; s++)
        if (slots[s].state == VCPE_PACK_READY)
            slots[s].state = VCPE_PACK_FREE;
}

int vcpe_pack_state(int h) { Slot *sl = slot_of(h, 0); return sl ? sl->state : VCPE_PACK_FREE; }
void vcpe_pack_pin(int h, int on) { Slot *sl = slot_of(h, 0); if (sl) sl->pinned = on != 0; }
void vcpe_pack_priority(int h, int p) { Slot *sl = slot_of(h, 0); if (sl) sl->priority = p < 0 ? 0 : p > 100 ? 100 : p; }
void vcpe_pack_touch(int h) { Slot *sl = slot_of(h, 0); if (sl) sl->used = ++use_clock; }

uint8_t *vcpe_pack_mem(int h)
{
    int s;
    Slot *sl = slot_of(h, &s);
    return sl && sl->state == VCPE_PACK_READY ? slot_mem(s) : 0;
}

int vcpe_pack_count(int h)
{
    const uint8_t *m = vcpe_pack_mem(h);
    return m ? (int)((const VcpePackHeader *)m)->count : 0;
}

const VcpePackEntry *vcpe_pack_entry(int h, int i)
{
    const uint8_t *m = vcpe_pack_mem(h);
    if (!m || i < 0 || i >= (int)((const VcpePackHeader *)m)->count)
        return 0;
    return (const VcpePackEntry *)(m + ((const VcpePackHeader *)m)->table) + i;
}

/* entries are sorted by hash: binary search */
const void *vcpe_pack_get(int h, uint32_t hash, uint32_t *size, int *kind)
{
    uint8_t *m = vcpe_pack_mem(h);
    if (size) *size = 0;
    if (!m)
        return 0;
    const VcpePackHeader *hd = (const VcpePackHeader *)m;
    const VcpePackEntry *e = (const VcpePackEntry *)(m + hd->table);
    int lo = 0, hi = (int)hd->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (e[mid].hash == hash) {
            if (e[mid].off > hd->size || e[mid].size > hd->size - e[mid].off)
                return 0;   /* damaged entry */
            if (size) *size = e[mid].size;
            if (kind) *kind = e[mid].kind;
            slots[h & 0xFF].used = ++use_clock;
            return m + e[mid].off;
        }
        if (e[mid].hash < hash) lo = mid + 1;
        else hi = mid - 1;
    }
    return 0;
}

const void *vcpe_pack_find(int h, const char *name, uint32_t *size) { return vcpe_pack_get(h, fnv(name), size, 0); }

int vcpe_pack_line(char *out, int n)
{
    int order[VCPE_PACK_MAX_SLOTS], k = 0, free_n = 0;
    for (int s = 0; s < nslots; s++) {
        if (slots[s].state == VCPE_PACK_FREE) { free_n++; continue; }
        int j = k++;
        while (j > 0 && slots[order[j - 1]].priority < slots[s].priority) {   /* insertion sort, highest first */
            order[j] = order[j - 1];
            j--;
        }
        order[j] = s;
    }
    int len = snprintf(out, n, "packs:");
    for (int i = 0; i < k && len < n; i++) {
        const Slot *sl = &slots[order[i]];
        len += snprintf(out + len, n - len, "%s @0x%x %d%s%s", i ? "," : "", (unsigned)sl->off, sl->priority,
                        sl->pinned ? " pinned" : "", sl->state == VCPE_PACK_LOADING ? " loading" : "");
    }
    if (len < n)
        len += snprintf(out + len, n - len, " (%d free)", free_n);
    return len;
}
