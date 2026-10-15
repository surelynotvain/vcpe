/* VCPE memory for the 32 MB PSP-1000 (see vcpe_mem.h). */
#include <pspkernel.h>
#include <pspsuspend.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include "vcpe_sys.h"
#include "vcpe_mem.h"

int vcpe_mem_32mb(void)
{
    static int small = -1;
    if (small < 0)
        small = vcpe_heap_kb() < 36000;
    return small;
}

uint8_t *vcpe_volatile_mem(uint32_t *size)
{
    static int tried;
    static void *mem;
    static int msize;
    if (!tried) {
        tried = 1;
        int st = vcpe_step_begin("Locking volatile memory (4096 KB)");
        if (sceKernelVolatileMemTryLock(0, &mem, &msize) < 0 || !mem || msize <= 0) {
            mem = 0;
            msize = 0;
        }
        vcpe_step_end(st, mem != 0);
    }
    if (size)
        *size = (uint32_t)msize;
    return (uint8_t *)mem;
}

static int peak;
int vcpe_mem_used_kb(void)
{
    struct mallinfo mi = mallinfo();
    if ((int)mi.uordblks > peak)
        peak = mi.uordblks;
    return mi.uordblks / 1024;
}

int vcpe_mem_peak_kb(void)
{
    vcpe_mem_used_kb();
    return peak / 1024;
}

/* ------------------------------------------------------------------ 4.0 */
int vcpe_mem_largest_free_kb(void) { return vcpe_heap_kb(); }

int vcpe_arena_init(VcpeArena *a, void *mem, uint32_t size)
{
    a->own = 0;
    if (!mem && size && !(mem = a->own = memalign(64, size)))
        size = 0;
    a->base = mem;
    a->size = size;
    a->used = a->peak = 0;
    return mem ? 0 : -1;
}

void *vcpe_arena_alloc(VcpeArena *a, uint32_t size, uint32_t align)
{
    if (align < 4)
        align = 4;
    uint32_t at = (a->used + align - 1) & ~(align - 1);
    if (!a->base || at + size > a->size || at + size < at)
        return 0;
    a->used = at + size;
    if (a->used > a->peak)
        a->peak = a->used;
    return a->base + at;
}

void vcpe_arena_free_all(VcpeArena *a)
{
    free(a->own);
    a->own = a->base = 0;
    a->size = a->used = 0;
}

/* modules a game may not link: weak, read only when present */
extern int vcpe_spr_loads __attribute__((weak)), vcpe_spr_fails __attribute__((weak));
int vcpe_spr_resident_kb(void) __attribute__((weak));
extern int vcpe_stat_reload_fail __attribute__((weak)), vcpe_stat_dlist_peak_kb __attribute__((weak));
extern int vcpe_stat_dlist_full __attribute__((weak));

int vcpe_mem_report(char *out, int n, const char *tag)
{
    struct mallinfo mi = mallinfo();
    int used = mi.uordblks / 1024, holes = mi.fordblks / 1024, largest = vcpe_heap_kb();
    if (used * 1024 > peak)
        peak = used * 1024;
    int len = snprintf(out, n, "mem %s: used %d KB (peak %d) largest free %d KB, holes %d KB", tag ? tag : "", used,
                       peak / 1024, largest, holes);
    if (vcpe_spr_resident_kb && len < n)
        len += snprintf(out + len, n - len, ", sprites %d KB (%d loads, %d failed)", vcpe_spr_resident_kb(),
                        &vcpe_spr_loads ? vcpe_spr_loads : 0, &vcpe_spr_fails ? vcpe_spr_fails : 0);
    if (&vcpe_stat_dlist_peak_kb && len < n)
        len += snprintf(out + len, n - len, ", dlist peak %d KB%s", vcpe_stat_dlist_peak_kb,
                        &vcpe_stat_dlist_full && vcpe_stat_dlist_full ? " FULL" : "");
    if (&vcpe_stat_reload_fail && vcpe_stat_reload_fail && len < n)
        len += snprintf(out + len, n - len, ", %d reloads failed", vcpe_stat_reload_fail);
    return len;
}
