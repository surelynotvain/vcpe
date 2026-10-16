# Upgrading to VCPE 4.7

4.7 includes the 4.1 tooling and the memory, runtime and loading improvements developed afterward.
Keep an existing game on its tested engine version until choosing to upgrade it. This release uses the original
standard loading screen and local two-slot savedata API.

## Build integration

1. Back up the game's engine selection, source and Makefile. Point `VCPE` at a checkout or pinned copy of 4.7.
2. Keep the game's existing `VCPE_MODULES`. Delete old engine object directories and rebuild so stale headers
   cannot leave mixed-version objects. Use the [4.0 guide](upgrading-to-4.0.md) if upgrading from 3.x.
3. Run the game's regression scenarios and inspect frame differences, load errors and timing on its target console.
   This release's engine-only checks cannot replace testing the game's own assets and systems.

Existing APIs remain available. The header-only `vcpe_store.h` and `vcpe_spatial.h` do not add link dependencies.
Components retain their contiguous-array interface; explicit component budgets must cover the game's instantiated
prefabs. Existing code that writes transforms directly remains supported by cache invalidation checks.

## Optional modules

| Module | Adoption |
|---|---|
| `vcpe_adapt` | Link it, then call `vcpe_adapt_enable(1)` to opt in; configure reserves and cosmetic floor for the game |
| `vcpe_diag` | Link for frame-percentile and CPU/GPU-wait/I/O/audio/allocation diagnostics |
| `vcpe_jobs` | Priority reservations, cooperative processing or explicit worker servers |
| `vcpe_pack` | Add `vcpe_jobs`; initialize a bounded pool and load converter-built asset packs |
| `vcpe_prefetch` | Add `vcpe_pack vcpe_jobs`; configure a bounded asset/scene table, budget and reserve |
| `vcpe_ultra` | Event bus, blackboard, live tuning and bounded utility structures |
| `vcpe_lod` | Build LOD data with `tools/vcpe_lod.py`; use it through `vcpe_3d` or the standalone picker |
| `vcpe_stream` | Optional intro/loop ADPCM channels mixed through `vcpe_audio`; initialize audio first |

Read the module headers for ownership and failure rules. Predicted assets transfer ownership when taken; release
their pins when no longer needed. Background completion callbacks run on the main thread. Without worker servers,
jobs run cooperatively within an explicit budget. Avoid surprise blocking reads during gameplay.

Adaptation scales cosmetic effect drawing and LOD, preserving simulation and RNG state. CPU, I/O and audio pressure
do not cause unnecessary visual reduction. Batching, pipelining and lazy texture flushing keep their opt-in
behavior. Recheck game-specific GE-read memory lifetimes before enabling pipelining.

The memory review's configurable limits and effect-history changes are documented in
[memory-optimizations.md](memory-optimizations.md) and [adaptive-budgets.md](adaptive-budgets.md).
The font atlas now allocates its second page only when needed and recovers from failed page allocation.
New static storage in diagnostics, spatial indexing and the paged store is visible in their source definitions;
the ultra tools' approximate 33 KiB BSS is documented in [ultra-tools.md](ultra-tools.md).

## Verification

From the repository root:

```sh
sh tests/run.sh
sh blank/build.sh
PSP_EMU=/path/to/PPSSPPHeadless sh tests/selftest/run.sh
PSP_EMU=/path/to/PPSSPPHeadless sh tests/memory/demo.sh /tmp/vcpe47-adapt
```

Host tests require a C compiler, Python 3, Pillow, NumPy and Fontconfig. PSP builds and the default font-header
lookup use Podman and `ghcr.io/pspdev/pspdev:latest`; set `VCPE_STB_INCLUDE` to an installed stb header directory
to avoid the font test's container lookup. The PSP self-test accepts `VCPE_ROOT` for a different engine checkout.

Release checks exercise:

- Deterministic 120-step physics/contact hashes: 3,941,120 full-scan candidates versus 614,650 with the grid.
- Cached transforms, shared prefab maps, atomic allocation failures and stable-address paged growth.
- 600 draw-order comparisons and 10,000 conservative rotated/mirrored bounds.
- Font glyph output, lazy atlas pages, failed allocation retries and 20 full-table rehash stress passes.
- Async duplicate readiness, bounded waiters, stale handles, reservations, worker startup and prediction ownership.
- Diagnostics percentiles/failures and pressure-aware adaptation, effect simulation/RNG, memory and save recovery.
- Input edges, image/data converters, math, event/blackboard/tuning toolkit, asset packs and LOD.
- The synthetic 400-block mixer output matches the published 4.0 source byte for byte. Set
  `VCPE_AUDIO_BASELINE=/path/to/older/src/vcpe_audio.c` when running `tests/run.sh` to repeat that comparison.
- Standalone PSP PAK/math/save/input/mouse/loading/LOD checks, matching batch/pipeline snapshots and animated loading.

These are controlled fixtures and standalone emulator checks, not a full-game regression claim. Format-width
warnings in the archived 4.7 diagnostic/demo `printf` calls remain visible in PSP build logs. No new real-console
tests are claimed. GE texture-cache behavior, real storage, sleep/resume and audio timing still need testing on
physical PSP models with the game's workload.
