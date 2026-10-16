#include <stdio.h>
#include <string.h>
#include "vcpe_adapt.h"
#if defined(__psp__)
#include <pspkernel.h>
static uint32_t now_us(void) { return sceKernelGetSystemTimeLow(); }
#else
#include <time.h>
static uint32_t now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000000u + t.tv_nsec / 1000);
}
#endif

/* Optional engine modules; linking this controller alone requires none of them. */
int vcpe_mem_available_kb(void) __attribute__((weak));
int vcpe_gfx_in_frame(void) __attribute__((weak));
uint32_t particles_trim(void) __attribute__((weak));
uint32_t render_trim(void) __attribute__((weak));
uint32_t vcpe_spr_trim(uint32_t bytes, int pages, uint32_t idle_frames) __attribute__((weak));
extern int vcpe_autotest __attribute__((weak));

static VcpeAdaptConfig cfg;
static VcpeAdaptStats st = {.scale256=256, .available_kb=-1};
static int configured, busy, pending_phase = -1, warmup;
static uint32_t bad, good, cooldown, frames;
static struct { VcpeAdaptWork fn; void *user; } work[8];
static unsigned work_cursor;
static uint32_t costs[VCPE_COST_COUNT];
void vcpe_adapt_cost(int d,uint32_t n)
{
    if (st.enabled && d>=0 && d<VCPE_COST_COUNT) costs[d]=UINT32_MAX-costs[d]<n?UINT32_MAX:costs[d]+n;
}

void vcpe_adapt_defaults(VcpeAdaptConfig *c)
{
    *c = (VcpeAdaptConfig){.target_us=16683, .slow_frames=20, .recover_frames=240, .cooldown_frames=90,
        .probe_frames=60, .play_budget_us=250, .load_budget_us=5000, .min_scale256=128, .step256=32,
        .reserve_kb=2048, .flags=VCPE_ADAPT_EFFECTS | VCPE_ADAPT_LOD | VCPE_ADAPT_TRIM};
}

int vcpe_adapt_config(const VcpeAdaptConfig *c)
{
    if (!c || c->target_us < 1000 || c->target_us > 1000000 || !c->slow_frames || !c->recover_frames ||
        !c->probe_frames || c->min_scale256 < 32 || c->min_scale256 > 256 || c->step256 < 1 || c->step256 > 256 ||
        c->reserve_kb < 0 || c->reserve_kb > 1048576 || c->play_budget_us > 2000 || c->load_budget_us > 100000 ||
        (c->flags & ~(VCPE_ADAPT_EFFECTS | VCPE_ADAPT_LOD | VCPE_ADAPT_TRIM)))
        return -1;
    cfg = *c; configured = 1;
    if (st.scale256 < cfg.min_scale256) st.scale256 = cfg.min_scale256;
    bad = good = cooldown = 0;
    return 0;
}

void vcpe_adapt_enable(int on)
{
    if (!configured) { VcpeAdaptConfig c; vcpe_adapt_defaults(&c); vcpe_adapt_config(&c); }
    st.enabled = on != 0;
    if (st.enabled) st.available_kb = vcpe_mem_available_kb ? vcpe_mem_available_kb() : -1;
    st.scale256 = 256; st.reason = VCPE_ADAPT_STABLE; st.ema_us = 0;
    bad = good = cooldown = frames = 0;
    warmup = 8; pending_phase = -1;
    memset(costs,0,sizeof costs); memset(st.cost_us,0,sizeof st.cost_us); st.bottleneck=VCPE_BOTTLENECK_UNKNOWN;
}

const VcpeAdaptStats *vcpe_adapt_stats(void) { return &st; }
int vcpe_adapt_effect_scale(void) { return st.enabled && (cfg.flags & VCPE_ADAPT_EFFECTS) ? st.scale256 : 256; }
float vcpe_adapt_lod_factor(void) { return st.enabled && (cfg.flags & VCPE_ADAPT_LOD) ? 256.0f / st.scale256 : 1; }

int vcpe_adapt_work_add(VcpeAdaptWork fn, void *user)
{
    if (!fn || busy) return -1;
    for (int i = 0; i < 8; i++)
        if (!work[i].fn) { work[i].fn = fn; work[i].user = user; return i; }
    return -1;
}
void vcpe_adapt_work_remove(int id) { if (!busy && id >= 0 && id < 8) memset(&work[id], 0, sizeof work[id]); }

static void reclaim_add(uint32_t bytes)
{
    st.reclaimed_bytes = UINT32_MAX - st.reclaimed_bytes < bytes ? UINT32_MAX : st.reclaimed_bytes + bytes;
}

/* Runs only between complete frames, one small work unit at a time. Built-in trimming is limited to cold,
 * independently allocated sprite pages and unused CPU histories; pinned packs, GE lists and live particles stay. */
static void maintain(int phase)
{
    if (!st.enabled || busy) return;
    if (vcpe_gfx_in_frame && vcpe_gfx_in_frame()) { pending_phase = phase; return; }
    uint32_t budget = phase >= VCPE_ADAPT_LOAD_BEGIN ? cfg.load_budget_us : cfg.play_budget_us;
    if (!budget) return;
    busy = 1;
    uint32_t start = now_us();
    if ((cfg.flags & VCPE_ADAPT_TRIM) && phase != VCPE_ADAPT_PLAY && phase != VCPE_ADAPT_LOAD_WORK) {
        if (particles_trim) reclaim_add(particles_trim());
        if (render_trim && now_us() - start < budget) reclaim_add(render_trim());
        /* An arena or all-resident cache cannot return heap bytes, so vcpe_spr_trim leaves it alone. Load-end
         * does not evict pages just preloaded for the next scene. At most two cold pages in a gameplay pass. */
        if (phase == VCPE_ADAPT_PRESSURE && vcpe_spr_trim && now_us() - start < budget)
            reclaim_add(vcpe_spr_trim(128u * 1024, 2, 120));
    }
    for (unsigned n = 0; n < 8 && now_us() - start < budget; n++) {
        unsigned i = work_cursor++ & 7;
        if (work[i].fn) {
            uint32_t spent = now_us() - start;
            if (spent < budget) reclaim_add(work[i].fn(phase, budget - spent, work[i].user));
            break;
        }
    }
    st.last_work_us = now_us() - start;
    if (st.last_work_us > budget) st.budget_overruns++;
    st.maintenance_runs++;
    busy = 0;
}

static void change(int scale, int reason)
{
    if (scale < cfg.min_scale256) scale = cfg.min_scale256;
    if (scale > 256) scale = 256;
    if (scale != st.scale256) { st.scale256 = scale; st.changes++; }
    st.reason = reason; cooldown = cfg.cooldown_frames; bad = good = 0;
}

void vcpe_adapt_sample(uint32_t elapsed_us, int available_kb)
{
    if (!st.enabled || busy) return;
    if (pending_phase >= 0) { int p = pending_phase; pending_phase = -1; maintain(p); }
    if (st.loading) return;
    st.available_kb = available_kb;
    for (int d=0;d<VCPE_COST_COUNT;d++) {
        st.cost_us[d]=(uint32_t)(((uint64_t)st.cost_us[d]*7+costs[d])/8);
    }
    st.bottleneck=VCPE_BOTTLENECK_UNKNOWN;
    if (costs[VCPE_COST_AUDIO]) st.bottleneck=VCPE_BOTTLENECK_AUDIO;
    else if (st.cost_us[VCPE_COST_IO]>cfg.target_us/3) st.bottleneck=VCPE_BOTTLENECK_IO;
    else if (st.cost_us[VCPE_COST_CPU]>cfg.target_us/2 &&
             (uint64_t)st.cost_us[VCPE_COST_CPU]>(uint64_t)st.cost_us[VCPE_COST_GPU]+st.cost_us[VCPE_COST_RENDER])
        st.bottleneck=VCPE_BOTTLENECK_CPU;
    else if ((uint64_t)st.cost_us[VCPE_COST_GPU]+st.cost_us[VCPE_COST_RENDER]>cfg.target_us/4)
        st.bottleneck=VCPE_BOTTLENECK_RENDER;
    memset(costs,0,sizeof costs);
    /* Long pauses/suspend/breakpoints are not a normal frame-time sample. Resume needs fresh history. */
    if (!elapsed_us || elapsed_us > 250000) { st.ema_us = 0; bad = good = 0; warmup = 8; return; }
    st.ema_us = st.ema_us ? (uint32_t)(((uint64_t)st.ema_us * 7 + elapsed_us) / 8) : elapsed_us;
    if (warmup) { warmup--; return; }
    frames++;
    if (cooldown) cooldown--;
    int pressure = available_kb >= 0 && available_kb < cfg.reserve_kb;
    int healthy_mem = available_kb < 0 || available_kb >= cfg.reserve_kb * 2;
    int slow = st.ema_us > cfg.target_us + cfg.target_us / 8;
    int healthy = st.ema_us <= cfg.target_us + cfg.target_us / 32 && healthy_mem;
    if (slow && !pressure && st.bottleneck>=VCPE_BOTTLENECK_CPU) {
        /* Drawing fewer effects cannot repair physics, blocked I/O or an audio starvation event. */
        st.reason=st.bottleneck==VCPE_BOTTLENECK_CPU?VCPE_ADAPT_CPU_BUSY:
            st.bottleneck==VCPE_BOTTLENECK_IO?VCPE_ADAPT_IO_BUSY:VCPE_ADAPT_AUDIO_BUSY;
        bad=good=0; return;
    }
    if (pressure || slow) { if (bad < cfg.slow_frames) bad++; good = 0; }
    else if (healthy) { if (good < cfg.recover_frames) good++; bad = 0; }
    else { bad = good = 0; }
    if (!cooldown && bad >= cfg.slow_frames)
        change(st.scale256 - cfg.step256, pressure ? VCPE_ADAPT_LOW_MEMORY : VCPE_ADAPT_SLOW);
    else if (!cooldown && good >= cfg.recover_frames)
        change(st.scale256 + cfg.step256, VCPE_ADAPT_RECOVER);
    /* Cheap, infrequent housekeeping. Memory samples never binary-search malloc. */
    if (frames % cfg.probe_frames == 0)
        maintain(pressure ? VCPE_ADAPT_PRESSURE : VCPE_ADAPT_PLAY);
}

void vcpe_adapt_frame(uint32_t elapsed_us)
{
    if (!st.enabled || busy) return;
    if (&vcpe_autotest && vcpe_autotest) { memset(costs,0,sizeof costs); return; }
    int available = st.available_kb;
    if ((frames % cfg.probe_frames == 0 || available < 0) && vcpe_mem_available_kb)
        available = vcpe_mem_available_kb();
    vcpe_adapt_sample(elapsed_us, available);
}

void vcpe_adapt_loading(int active)
{
    st.loading = active != 0;
    st.ema_us = 0; bad = good = 0; warmup = 8;
    memset(costs,0,sizeof costs); memset(st.cost_us,0,sizeof st.cost_us); st.bottleneck=VCPE_BOTTLENECK_UNKNOWN;
    if (st.enabled) maintain(active ? VCPE_ADAPT_LOAD_BEGIN : VCPE_ADAPT_LOAD_END);
}

void vcpe_adapt_loading_tick(void)
{
    if (st.loading) maintain(VCPE_ADAPT_LOAD_WORK);
}

int vcpe_adapt_line(char *out, int n)
{
    static const char *reason[] = {"stable", "slow frames", "low memory", "recovering", "CPU bound", "I/O bound", "audio pressure"};
    return snprintf(out, n, "adapt %s: %d%%, %s, frame %u us, free ~%d KB, reclaimed %u KB, work %u us (%u overruns)",
        st.enabled ? "on" : "off", st.scale256 * 100 / 256, reason[st.reason], (unsigned)st.ema_us,
        st.available_kb, (unsigned)(st.reclaimed_bytes / 1024), (unsigned)st.last_work_us, (unsigned)st.budget_overruns);
}
