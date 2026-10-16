#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "vcpe_mem.h"

int main(void)
{
    _Alignas(64) uint8_t mem[256];
    VcpeArena a;
    assert(vcpe_arena_init(&a, mem + 1, 255) == 0);
    void *p = vcpe_arena_alloc(&a, 16, 64);
    assert(p && !((uintptr_t)p & 63) && a.used == 79);
    uint32_t mark = vcpe_arena_mark(&a), peak = a.peak;
    assert(!vcpe_arena_alloc(&a, 1, 6) && a.used == mark);
    assert(!vcpe_arena_alloc(&a, UINT32_MAX, 64) && a.used == mark && a.peak == peak);
    assert(!vcpe_arena_alloc(&a, 16, 0x80000000u) && a.used == mark);
    assert(vcpe_arena_alloc(&a, 16, 1));
    vcpe_arena_release(&a, mark); assert(a.used == mark && a.peak > peak);
    vcpe_arena_reset(&a); assert(a.used == 0);
    assert(vcpe_arena_alloc(&a, 192, 64));
    assert(!vcpe_arena_alloc(&a, 1, 4));
    vcpe_arena_free_all(&a); assert(!a.base && !a.used);
    assert(vcpe_arena_init(&a, 0, 256) == 0);
    assert(vcpe_arena_alloc(&a, 256, 64));
    assert(!vcpe_arena_alloc(&a, 1, 4));
    vcpe_arena_free_all(&a);
    puts("arena alignment, overflow, marks, exact fit and ownership checks passed");
}
