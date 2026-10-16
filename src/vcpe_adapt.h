/* Optional automatic frame/memory budgeting. Link vcpe_adapt and call vcpe_adapt_enable(1).
 * No scaling or maintenance happens until enabled. Main thread only; does not change simulation or resolution. */
#ifndef VCPE_ADAPT_H
#define VCPE_ADAPT_H
#include <stdint.h>
#include "vcpe_diag.h"
enum { VCPE_ADAPT_EFFECTS = 1, VCPE_ADAPT_LOD = 2, VCPE_ADAPT_TRIM = 4 };
enum { VCPE_ADAPT_PLAY, VCPE_ADAPT_PRESSURE, VCPE_ADAPT_LOAD_BEGIN, VCPE_ADAPT_LOAD_END, VCPE_ADAPT_LOAD_WORK };
enum { VCPE_ADAPT_STABLE, VCPE_ADAPT_SLOW, VCPE_ADAPT_LOW_MEMORY, VCPE_ADAPT_RECOVER,
       VCPE_ADAPT_CPU_BUSY, VCPE_ADAPT_IO_BUSY, VCPE_ADAPT_AUDIO_BUSY };
enum { VCPE_BOTTLENECK_UNKNOWN, VCPE_BOTTLENECK_RENDER, VCPE_BOTTLENECK_CPU, VCPE_BOTTLENECK_IO, VCPE_BOTTLENECK_AUDIO };
typedef struct {
    uint32_t target_us, slow_frames, recover_frames, cooldown_frames, probe_frames;
    uint32_t play_budget_us, load_budget_us;
    int min_scale256, step256, reserve_kb, flags;
} VcpeAdaptConfig;
typedef struct {
    int enabled, scale256, reason, available_kb, loading;
    uint32_t ema_us, changes, maintenance_runs, reclaimed_bytes, last_work_us, budget_overruns;
    int bottleneck;
    uint32_t cost_us[VCPE_COST_COUNT];
} VcpeAdaptStats;
void vcpe_adapt_defaults(VcpeAdaptConfig *c);
int vcpe_adapt_config(const VcpeAdaptConfig *c); /* -1 invalid; preserves current config on failure */
void vcpe_adapt_enable(int on);
const VcpeAdaptStats *vcpe_adapt_stats(void);
int vcpe_adapt_line(char *out, int n);
int vcpe_adapt_effect_scale(void);   /* 128..256 by default, 256 when disabled */
float vcpe_adapt_lod_factor(void);   /* 1..2 by default, 1 when disabled */

/* Cooperative micro work: one callback per maintenance pass, round-robin, fixed 8-entry registry. Return bytes
 * actually freed (0 for prewarming). Honor budget_us: the engine cannot preempt a callback. No GPU commands,
 * nested loading, changing the registry, or game-state changes here. Only reclaim memory the callback owns. */
typedef uint32_t (*VcpeAdaptWork)(int phase, uint32_t budget_us, void *user);
int vcpe_adapt_work_add(VcpeAdaptWork work, void *user);
void vcpe_adapt_work_remove(int id);

/* Automatic hooks called by gfx/load, or manually by a host without those modules. frame() includes update,
 * rendering and pacing; it skips autotest timing. sample() accepts deterministic inputs, available_kb -1 unknown.
 * Do not feed manual samples as well as automatic frame hooks. Loading stalls never become slow-play samples. */
void vcpe_adapt_frame(uint32_t elapsed_us);
void vcpe_adapt_sample(uint32_t elapsed_us, int available_kb);
void vcpe_adapt_loading(int active);
void vcpe_adapt_loading_tick(void); /* one cooperative work unit during a loading-screen progress tick */
void vcpe_adapt_cost(int domain,uint32_t amount); /* main-thread costs; see vcpe_diag.h */
#endif
