#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "vcpe_adapt.h"
#include "vcpe_lod.h"
static int in_frame, memory_kb = 8192, trim_calls, work_calls, last_phase;
int vcpe_autotest;
int vcpe_gfx_in_frame(void) { return in_frame; }
int vcpe_mem_available_kb(void) { return memory_kb; }
uint32_t particles_trim(void) { trim_calls++; return 1024; }
uint32_t render_trim(void) { trim_calls++; return 2048; }
uint32_t vcpe_spr_trim(uint32_t n, int pages, uint32_t idle)
{
    assert(n <= 128 * 1024 && pages <= 2 && idle >= 120);
    trim_calls++; return 65536;
}
static uint32_t work(int phase, uint32_t budget, void *user)
{
    assert(!in_frame && budget > 0 && user == &work_calls);
    assert(vcpe_adapt_work_add(work, user) == -1); /* registry mutation inside a callback is refused */
    last_phase = phase; work_calls++; return 17;
}
static void samples(int count, uint32_t us, int kb) { while (count--) vcpe_adapt_sample(us, kb); }
static uint32_t fair_work(int phase, uint32_t budget, void *user)
{
    (void)phase; (void)budget; (*(int *)user)++; return 0;
}
static uint32_t overrun_work(int phase, uint32_t budget, void *user)
{
    (void)phase; (void)user;
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    do { clock_gettime(CLOCK_MONOTONIC, &now); }
    while ((now.tv_sec-start.tv_sec)*1000000 + (now.tv_nsec-start.tv_nsec)/1000 < (long)budget + 2000);
    return 0;
}
int main(void)
{
    const VcpeAdaptStats *s = vcpe_adapt_stats();
    assert(!s->enabled && vcpe_adapt_effect_scale() == 256 && vcpe_adapt_lod_factor() == 1);
    int id = vcpe_adapt_work_add(work, &work_calls); assert(id >= 0);
    samples(300, 40000, 0); assert(!work_calls && !trim_calls && s->scale256 == 256);
    VcpeAdaptConfig c; vcpe_adapt_defaults(&c);
    c.target_us = 10000; c.probe_frames = 10;
    assert(vcpe_adapt_config(&c) == 0);
    c.min_scale256 = 0; assert(vcpe_adapt_config(&c) == -1); c.min_scale256 = 128;
    vcpe_adapt_enable(1);
    samples(60, 10000, 8192);
    samples(1, 50000, 8192); samples(80, 10000, 8192);
    assert(s->scale256 == 256); /* one hitch is not sustained overload */
    samples(400, 20000, 8192);
    assert(s->scale256 == 128 && s->reason == VCPE_ADAPT_SLOW);
    assert(vcpe_adapt_effect_scale() == 128 && vcpe_adapt_lod_factor() == 2);
    /* Verify that the real LOD picker responds, including callers using their own view. */
    VcpeLodLevel levels[3] = {{.err=0}, {.err=.15f}, {.err=.3f}};
    VcpeLodMesh m = {.nlevels=3, .lv=levels};
    VcpeLodView v = {.px_k=100, .thresh_px=1, .down_k=.75f};
    float center[3] = {0,0,10};
    assert(vcpe_lod_pick(&m, &v, center, 1, -1) == 1);
    uint32_t changes = s->changes;
    vcpe_adapt_loading(1);
    samples(600, 100000, 0); assert(s->changes == changes && s->scale256 == 128);
    assert(last_phase == VCPE_ADAPT_LOAD_BEGIN);
    int trims_before = trim_calls;
    vcpe_adapt_loading_tick();
    assert(last_phase == VCPE_ADAPT_LOAD_WORK && trim_calls == trims_before);
    vcpe_adapt_loading(0); assert(last_phase == VCPE_ADAPT_LOAD_END);
    samples(1600, 10000, 8192); assert(s->scale256 == 256);
    assert(vcpe_lod_pick(&m, &v, center, 1, -1) == 0);
    samples(500, 10000, 0);
    assert(s->scale256 == 128 && s->reason == VCPE_ADAPT_LOW_MEMORY && trim_calls > 0);
    changes = s->changes; samples(500, 10000, 3000);
    assert(s->changes == changes); /* memory recovery hysteresis: twice the reserve */
    vcpe_adapt_enable(1);
    vcpe_autotest = 1;
    for (int i=0; i<300; i++) vcpe_adapt_frame(40000);
    assert(s->scale256 == 256); /* host/emulator speed cannot change recordings */
    vcpe_autotest = 0;
    in_frame = 1; int calls = work_calls;
    vcpe_adapt_loading(1); assert(work_calls == calls);
    in_frame = 0; vcpe_adapt_frame(10000);
    assert(last_phase == VCPE_ADAPT_LOAD_BEGIN && work_calls == calls + 1);
    vcpe_adapt_loading(0); assert(last_phase == VCPE_ADAPT_LOAD_END);
    samples(30, 20000, 8192);
    samples(1, 1000000, 8192); assert(!s->ema_us); /* sleep/paused debugger excluded */
    vcpe_adapt_enable(0); assert(vcpe_adapt_effect_scale() == 256 && vcpe_adapt_lod_factor() == 1);
    calls = work_calls; vcpe_adapt_loading(1); vcpe_adapt_loading(0); assert(work_calls == calls);
    vcpe_adapt_work_remove(id);
    int served[8]={0}, ids[8];
    for (int i=0; i<8; i++) { ids[i]=vcpe_adapt_work_add(fair_work, &served[i]); assert(ids[i]>=0); }
    assert(vcpe_adapt_work_add(fair_work, served)==-1);
    vcpe_adapt_enable(1); vcpe_adapt_loading(1);
    for (int i=0; i<8; i++) vcpe_adapt_loading_tick();
    for (int i=0; i<8; i++) { assert(served[i]>=1); vcpe_adapt_work_remove(ids[i]); }
    vcpe_adapt_loading(0);
    id=vcpe_adapt_work_add(overrun_work,0); assert(id>=0);
    c.load_budget_us=1000; assert(vcpe_adapt_config(&c)==0);
    uint32_t overruns=s->budget_overruns;
    vcpe_adapt_loading(1); assert(s->budget_overruns>overruns && s->last_work_us>=1000);
    vcpe_adapt_work_remove(id); vcpe_adapt_loading(0); vcpe_adapt_enable(0);
    char line[256]; vcpe_adapt_line(line, sizeof line); puts(line);
    puts("adaptive overload/recovery, hysteresis, LOD, loading, budgets, safe boundaries and recording checks passed");
}
