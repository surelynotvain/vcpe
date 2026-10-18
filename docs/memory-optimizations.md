# Engine memory review, 2026-10-16

These changes are in the active `vcpe` engine. Version snapshots and installed game builds are unchanged.
These changes ship in VCPE 5.0; see `release-5.0.md` for verification and hardware limits.

## Changes and measured savings

| Area | Before | After |
| --- | --- | --- |
| Runtime particles | 1,372 bytes per component **capacity** slot, including 360 prefab reserve slots | 1,356 bytes per instantiated component at its high water mark |
| Runtime trails | 300 bytes per capacity slot, including 360 reserve slots | 300 bytes per instantiated component at its high water mark |
| Graphics batch staging | Separate 24 KiB textured and 16 KiB solid arrays | One 24 KiB union: **16 KiB less BSS** |
| Fixed-size save read | Two payload-sized heap temporaries | One payload-sized temporary plus a 512-byte stack validation buffer |
| Fixed-size save write | One payload-sized heap temporary | A 512-byte stack validation buffer; no payload-sized heap allocation |

The default runtime reserve alone formerly cost 601,920 bytes (587.8 KiB) for particle/trail histories.
The particle state also drops 16 bytes per instantiated emitter. Default visual limits remain 48 particles per
emitter and 24 trail points. No particle random calls, integration, burst timing, sorting or trail sampling changed.

In emulator regression runs, the existing runtime memory log fell by **588 KiB** in each of the four measured scene
samples. PSP object inspection independently measured `vcpe_gfx.o` BSS falling from 565,824 to 549,440 bytes (16 KiB),
with identical text/data sizes in the first memory-only pass. The later adaptive frame hook adds 88 bytes of
graphics code in the measured PSP build; the 16 KiB BSS saving remains. The heap measurement and BSS reduction
are separate quantities. The opt-in controller object adds 3,455 bytes of code, 48 bytes of data and 140 bytes
of BSS in the measured build (linked engine services have their own costs).
An engine rebuild is necessary for a game to receive these changes.

## Allocation behavior and compatibility

Particles and trails keep storage at the instantiated-component high water mark. Destroyed slots can be reused;
steady-state updates do not allocate. Growing that high water mark can call `realloc`, so this is a memory/latency
tradeoff for games continually instantiating new effect components. Create expected components behind the loading
screen when allocation latency matters. Particle resets reuse an existing block and clear playback; resetting a
scene with no particle components releases the particle block. Trail storage and the draw list retain their peak.

Growth never discards live effects. On allocation failure, existing entries still update/draw; only new entries or
draw-list items that did not fit are skipped. The next growth attempt can recover. `particles_alloc_fails` and
`render_alloc_fails` count these failures. `particles_memory_bytes()` and `render_memory_bytes()` report reserved
payload bytes (the latter includes trail histories and draw-list capacity), excluding allocator overhead.

`-DVCPE_PARTICLES_PER_SYSTEM=n` and `-DVCPE_TRAIL_POINTS=n` tune per-component limits. Minimums are one particle and
two trail points. Lowering them intentionally changes effect quality; defaults preserve existing rendering.

Save files and APIs remain compatible. Fixed-size reads validate both slots in chunks, then read and revalidate
the selected slot into one temporary before copying to the caller. This reads one payload again in exchange for
halving peak payload scratch memory, and keeps failed reads from modifying the caller's data. Writes validate
without allocating a payload buffer, including under heap pressure. `stdio` can still allocate its own buffers.
The variable-size save APIs being developed separately are outside this patch.

Arena allocations now align the returned address even when external memory starts unaligned. They reject
non-power-of-two alignments (values below four are normalized to four), exhaustion and 32-bit size wrap without
advancing the cursor.

## Verification

The follow-on automatic controller is described in [adaptive-budgets.md](adaptive-budgets.md). All source changes
are engine-side; disposable compatibility builds were kept under the engine test workspace. No original game
project was updated. Further work and validation are confined to VCPE and its own tests.

From the engine folder:

```sh
sh tests/memory/run.sh
# Optional: compare actual particle squares and trail vertices against a pre-change engine tree.
sh tests/memory/run.sh /path/to/pre-change-vcpe
# Sanitized host run (Clang's bundled sanitizer runtime works on the development host).
CC=clang CFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' sh tests/memory/run.sh
```

Tests cover 600 deterministic frames with bursts, loops, gravity, fading, stopping/restarting, emitter growth,
trail growth, inactive/destroyed components; failed allocations and recovery; no steady-state reallocations;
zero-emitter scenes; actual-address arena alignment and overflow; 131,073-byte saves with a nonaligned final chunk,
heap exhaustion, CRC/format checks, newest-slot selection, sequence wrap and empty saves.

PSP builds passed for two existing engine clients with the same warning counts as the baseline. All 14 recorded
game scenarios (532 captured frames) were byte-identical before/after. The PSP engine self-test passed, including
batched/unbatched and pipelined frame comparisons. These are emulator and host results; physical PSP memory and
frame-time measurements remain unverified.

## Remaining opportunities

- `vcpe_ttf` retains an 80 KiB duplicate glyph table while rehashing. In-place deletion could remove it, but needs
  font-page eviction and collision-chain stress tests before changing that cache.
- Component definition arrays still reserve slots for prefab instances. Removing those reserves needs a wider
  pointer-lifetime audit across scripts, animation and physics; it is separate from removing unused effect history.
- A shared budget for active particles could save more in scenes with many idle emitters, but needs an explicit
  exhaustion policy so one effect cannot silently steal another emitter's 48-particle allowance.
- Texture-cache slot sizes, display-list capacity and audio buffers remain configurable budget decisions. Reducing
  them globally without per-game workload measurements can cause reloads, full lists or audio underruns.
