# Automatic budgets during play and loading

Add `vcpe_adapt` to the game's `VCPE_MODULES` and call `vcpe_adapt_enable(1)` after system initialization.
Graphics and loading modules then drive it automatically. The controller is off by default, including when linked.
Blank Engine enables it and provides an **ADAPT** page with live status and an interactive demo.

## Dynamic detail

The controller measures intervals between presented frames, including update, rendering and pacing. A smoothed
average and sustained-overload window ignore isolated hitches. Loading frames and long pauses are excluded;
loading/pause recovery starts with fresh samples.

At the default 60 Hz target, 20 sustained slow samples reduce detail by 12.5 percentage points. Changes wait at
least 90 frames. Recovery requires 240 healthy samples per step, with memory above twice the low-memory reserve.
The default floor is 50%; target, floor, windows, cooldown and budgets are configurable.

- Particles draw a stable subset of existing particles. Simulation, emission and random calls continue unchanged,
  so drawing fewer particles cannot alter gameplay or the global random stream.
- Trails draw fewer intermediate points, retaining both endpoints and the complete history.
- Mesh LOD raises the caller's permitted pixel error by up to 2x at the default floor. Existing LOD hysteresis still
  applies, including views created outside `vcpe_3d_lod_view`.

This scales rendering detail. Simulation frequency, UI, framebuffer resolution, textures and collision geometry
are unchanged. Allocated particle simulation capacity does not shrink with detail. Games can use
`vcpe_adapt_stats()->scale256` for additional explicitly cosmetic detail choices.

## Memory and loading

Once per 60 gameplay samples, a cheap headroom estimate combines free user-partition memory with free blocks held
by malloc. It never binary-searches heap allocations during play. Fragmented space is included, so this cannot
guarantee one allocation of that size will fit. Explicit loading diagnostics can use `vcpe_mem_largest_free_kb()`.

Under pressure, maintenance shrinks unused particle/trail/draw-list capacity and releases at most two cold heap
sprite pages, up to 128 KiB per pass, unused for 120 sprite frames. Current/recent pages, all-resident slabs,
fixed cache arenas, live particles and pack pools stay protected. Arena slots cannot return heap bytes, so they
are left alone. A trimmed page can require a lazy reload later; normal `vcpe_spr_frame_begin()` calls still matter.

Loading begin/end shrink unused history/draw-list capacity. Load-end preserves newly preloaded textures.
The controller does not compact the whole heap or silently move game data. Reclaimed bytes count payload capacity,
excluding allocator overhead.

## Cooperative mini optimizations

Register small resumable cleanup or prewarm steps over memory/cache data the callback owns:

```c
static uint32_t maintenance(int phase, uint32_t budget_us, void *user)
{
    /* Advance one bounded step honoring budget_us. Return bytes actually freed,
       or 0 for prewarming. Use phase to distinguish play, pressure and loading. */
    return 0;
}
int task = vcpe_adapt_work_add(maintenance, &cache);
/* Before destroying cache: */
vcpe_adapt_work_remove(task);
```

Eight callback slots require no heap allocation; one is served per pass, round-robin. The shared budget defaults
to **250 us during play** and **5 ms during loading**. Gameplay passes run once per probe interval. Loading passes
run at begin/end and at most once per 50 ms of loading progress ticks. Progress work does not repeatedly trim
histories. Requests inside a graphics frame defer to a safe boundary; work runs on the main thread.

Budgets are cooperative. An allocator, driver, I/O operation or callback cannot be preempted. Work stops starting
when the budget is exhausted; observed overruns are counted. Callbacks must bound their own work, avoid GPU
commands/nested loading/game-state changes, and not mutate the callback registry while executing. Large conversions
and full-cache rebuilds belong in explicit loading steps or separate work reservations.

## Configuration and diagnostics

```c
VcpeAdaptConfig c;
vcpe_adapt_defaults(&c);
c.target_us = 33367;             /* a 30 Hz game */
c.min_scale256 = 192;            /* at least 75% cosmetic drawing */
c.reserve_kb = 3072;
c.flags = VCPE_ADAPT_EFFECTS | VCPE_ADAPT_LOD | VCPE_ADAPT_TRIM;
if (vcpe_adapt_config(&c) == 0)
    vcpe_adapt_enable(1);
char line[256];
vcpe_adapt_line(line, sizeof line);
```

Remove flags to preserve that subsystem's detail or disable built-in trimming. Invalid configuration returns -1
without changing the current configuration. Disable restores full cosmetic/LOD detail immediately. Re-enable starts
at full detail. Cumulative diagnostics are retained: scale, reason, frame average, estimated free memory,
reclaimed bytes, maintenance runs/time and budget overruns.

Autotests skip automatic wall-clock adaptation so emulator/host speed cannot alter recordings. Explicit
`vcpe_adapt_sample(us, available_kb)` supplies deterministic test inputs; -1 means unknown memory. Do not feed manual
samples alongside automatic hooks in normal gameplay. Other hosts can drive the public frame/loading hooks.

## Verification

`sh tests/memory/run.sh` tests overload/recovery, isolated hitches, hysteresis, pressure, loading/pause exclusion,
deferred work, callback safety, actual LOD selection, rendering-only effect scaling and cold-page sprite reclamation.
AddressSanitizer and UndefinedBehaviorSanitizer pass.

The production particle fixture draws **45 -> 22 squares** and a trail **48 -> 26 vertices** at 50%, retaining
simulation state and RNG. The PPSSPP Blank Engine demo reaches 50% under injected overload, restores 100% under
injected recovery, and reclaims a real **64 KiB** cache at loading. Demo work measured single-digit microseconds
with no overruns in the emulator. This does not establish physical PSP frame-rate gains or maintenance latency.

On ADAPT: X toggles; Up/Down inject labelled overload/recovery; Triangle creates/reclaims the demo cache at loading.
Autotest commands: `adapt 1`, `adaptfeed 35000 8192 500`, `adaptcheck 128`, `adaptload`,
`adaptfeed 16683 8192 1600`, `adaptcheck 256`.

Reproduce the standalone engine demo without any game project:

```sh
PSP_EMU=/path/to/PPSSPPHeadless sh tests/memory/demo.sh /tmp/vcpe-adapt-demo
```

It rebuilds only Blank Engine and verifies full/reduced/recovered detail, 64 KiB reclamation, and periodic work
during a short synthetic load (four maintenance passes). Four raw screenshots and build/runtime logs are kept.
