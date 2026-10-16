# VCPE changelog

## 4.7 (2026-10-16): automatic budgets, runtime optimization and loading

- Collision broadphase preserves contact order and state in the deterministic fixture while reducing candidate checks.
- Cached world transforms and render ordering skip unchanged work; bounds remain conservative under rotation/mirroring.
- Unused component kinds avoid generic prefab reserves; explicit budgets, shared prefab remaps and a stable-address
  paged record store reduce allocations while preserving existing component interfaces.
- TTF allocates its second 256 KiB glyph page on demand and removes 80 KiB of static rebuild scratch.
- Async packs notify duplicate requesters only when data is ready; waiters and generation/overflow failures are
  bounded and tested. Worker startup wakes jobs already queued. Optional prefetch predicts and pins bounded assets.
- `vcpe_diag` reports frame average/p95/p99/worst, CPU/GPU-wait/I/O/audio pressure and per-module allocation failures.
  Adaptation distinguishes CPU/I/O/audio pressure from rendering pressure before reducing cosmetic detail.
- Optional `vcpe_stream` (4.5 iteration) supports simultaneous ADPCM music/ambience channels, intro/loop samples,
  pitch, fades and resume positions. `vcpe_save_*_var` supports variable-size checked local payloads.
- Includes the 4.1 tools below and a standalone Blank Engine with ADAPT/BENCH demonstrations.
- Public snapshot includes engine-only host and PSP self-tests with portable paths. Hardware behavior and full-game
  regressions for the complete 4.7 snapshot remain unverified; see `docs/upgrading-to-4.7.md`.

Memory and adaptation changes:

- New optional `vcpe_adapt`: automatic frame/memory budgeting, gradual cosmetic particle/trail and mesh LOD scaling,
  slow quality recovery, cold-page reclamation, and cooperative micro work during play and loading.
  Existing games keep full detail until they link and enable it. Blank Engine has a live ADAPT page and demo.
- Runtime particle pools and trail histories track instantiated components instead of the world's unused prefab
  reserve. Live histories survive growth; failed allocations leave existing effects and draw-list storage valid.
  Particle playback and the default 48 particles / 24 trail points are unchanged; both limits can be configured.
- Particle playback flags use a byte mask; effect/draw-list byte counts and allocation-failure counters are exposed
  in `w_render.h`. Same-size particle resets reuse their block. Empty particle scenes allocate nothing.
- Graphics batches share storage for textured and solid vertices: 16 KiB less BSS with the default batch size.
- Fixed-size save validation uses 512-byte chunks: writes drop their full-size scratch allocation; reads use one
  save-sized temporary instead of two. Existing save format, CRC checks and two-slot selection are preserved.
- Arena allocations align the actual address of caller-supplied memory and reject invalid alignment/overflow.
- See `docs/memory-optimizations.md` for measurements, tradeoffs and repeatable host checks.
- See `docs/adaptive-budgets.md` for automatic tuning, thresholds, loading work and diagnostics.

## 4.1 (2026-10-15): ultra tools, and the engine explained in its own source

- New module `vcpe_ultra` (opt-in; docs/ultra-tools.md):
  - **Event bus** between the engine and the game: `vcpe_ev_post` / `vcpe_ev_on` / `vcpe_ev_dispatch`, fixed
    queues, in-order delivery on the main thread, a lock-free ring for posts from another thread. The engine's
    own events: resume after sleep, loading screen begin/end, late frame (missed vblank), low memory, blackboard and
    tuning changes.
  - **Blackboard**: named int/float/string values shared by the engine, the game and test scripts, with change
    events and a dump.
  - **Live tuning**: registered variables follow a `name = value` text file (TUNE.TXT, or `host0:` over PSPLink),
    re-read when it changes, clamped to their range; `vcpe_tune_save` writes the current values.
  - **Toolkit**: FNV-1a hashes, 16.16 fixed point, ring buffer, O(1) object pool, drift-free timers, a profiler
    with named scopes.
  - Autotest commands `tune`, `bb`, `bbcheck`, `ev`. Host test `tests/host/ultra_test.c` (20 checks) and 3 self-test
    checks on the PSP.
- New module `vcpe_jobs` (docs/reservations-and-packs.md): reservations for background work. Clients (engine
  modules, game systems) book reservations in one line; servers (worker threads) hand them out by automatic
  priority (client priority + urgency + waiting time); done callbacks run on the main thread. Without servers the
  main thread serves them within a time budget per frame.
- New module `vcpe_pack` + `tools/vcpe_packer.py`: compact loading. Asset packs load with one read into slots of a
  fixed pool and unload instantly; when the pool is full the priority list evicts the lowest-priority, least
  recently used pack (pinned and loading packs stay). Background loads go through a reservation.
- Blank Engine (`blank/`, docs/blank-engine.md): the engine compiled with no game, a UI over the engine picture
  with technical information and live demos of the reservations (3 at once, 2 clients, 2 servers) and the ultra
  tools.
- `vcpe_pak_path()`.
- New module `vcpe_lod` + `tools/vcpe_lod.py` (docs/lod.md): LOD meshes. Offline, triangles are joined by
  quadric half-edge collapses into up to 8 levels that share one vertex array (each level a prefix), with a
  measured error per level; seams and open edges stay put. At runtime `vcpe_lod_pick` turns that error into pixels
  for the current camera and picks the coarsest level under the threshold (1 px by default), with hysteresis.
  `vcpe_3d_lod_view` / `vcpe_3d_draw_lod` in `vcpe_3d`; stats `vcpe_lod_stat_full` / `_drawn`. The module has no PSP
  headers (first piece of the move away from PSPSDK: only `vcpe_3d` talks to the GPU). Host test
  `tests/host/lod_test.sh` (17 checks + 2 tool checks); self-test scene 3 compares a LOD frame with full detail.
- Host test `tests/host/jobs_pack_test.sh` (20 checks, real threads); self-test checks on the PSP.
- Comments: every module of `src/` and `runtime/` now explains how it works for someone new to the PSP (the GE and
  display lists, the texture cache, IMA-ADPCM, coroutine hand-over, the LRU page cache, Unity's update order...),
  about 1,000 lines. Code unchanged: every engine object file is byte-identical to 4.0 and every file's code
  matches token for token.
- The 4.1 development recordings matched 4.0 apart from wall-clock-driven frames.

## 4.0 (2026-10-15): smoothness, memory and robustness

An engine review (smoothness, audio, threads, memory, robustness) checked against the code. docs/upgrading-to-4.0.md
lists every point and what was done with it. Every game in the engine's frame-recording regression draws frame-identical to 3.5
(apart from one scenario whose Lua scripts read `os.time`, which differs between 3.5 runs too), with the same build warnings.
New behaviour is either internal (fewer GE commands for the same picture) or opt-in.

Faster by default (same picture):
- Text binds its texture only when it changes. The HUD font, TTF glyph pages and the loading animation claim a
  texture identity (`vcpe_tex_claim` / `vcpe_tex_bound`) instead of re-sending CLUT + TexMode + TexImage + TexFlush
  for every string. 300 HUD strings in a frame bind the font once (selftest). The loading boot log no longer binds
  15 times per redraw. Any other bind (`vcpe_bind_page`, sprites, `vcpe_unbind`, `vcpe_gfx_state_reset`, a new frame)
  forgets it, so games that bind raw textures and call `vcpe_unbind()` are safe as before.
- Sprites: the rotation sin table is built in `vcpe_spr_init`, not on the first rotated draw. The page cache keeps
  an LRU list (eviction is O(1), not a scan of every page) and waits for the GE once per eviction pass, not per page.
- TTF: a recycled glyph page clears and writes back only the rows it used, not all 256 KB.
- PAK reads skip the seek when the file is already at that offset (sequential loads).
- Loading screen: redraws every 50 ms (was 33 ms). Each redraw waits for a vblank, so loads finish sooner;
  `vcpe_loading_redraw_ms(ms)` changes it.
- After a sleep, bundles in volatile memory are read again behind the loading screen when the game links
  `vcpe_load`, instead of a frozen frame. `vcpe_resume_pending()` / `vcpe_bundles_reload()` let a game do it at a
  moment it chooses. A failed read is counted (`vcpe_stat_reload_fail`) and tried again at the next 2 frames.
- Coroutines: the threads of the first 8 slots stay parked after their coroutine ends and are reused
  (`-DVCPE_CO_POOL=n`), so scripted sequences no longer create and delete threads while playing. `vcpe_co_reset`
  still kills coroutines stopped midway; `vcpe_co_shutdown` also ends the parked threads.
- `vcpe_bundle_slab` keeps its block when the new range fits in it.

Opt-in:
- `vcpe_opaque(1)`: blending off for spans the game knows are opaque (tilemaps, backgrounds), so the GE does not
  read the frame buffer for them. Every frame starts with blending on.
- `vcpe_gfx_lazy_texflush(1)`: skip `sceGuTexFlush` on binds when no texels were uploaded since the last flush.
  Uploads are tracked by the engine (`vcpe_tex_uploaded`). Off until measured on a real PSP: PPSSPP ignores the GE
  texture cache, so recordings can't prove it.
- `vcpe_spr_arena(mem, size)`: the sprite page cache in fixed slots inside one block (engine-allocated or the
  game's, e.g. the volatile partition): no heap traffic while playing, a bounded worst case for the PSP-1000.
- `vcpe_spr_load_all` falls back to one allocation per page when no single block is free.
- `vcpe_ttf_prewarm(font, px, chars)`: rasterize what the next scene shows behind the loading screen.
- `vcpe_arena_*` (vcpe_mem): a bump allocator for per-scene data, reset at the next scene switch.
- `vcpe_dt()`: frame time from counted vblanks (one vblank in autotest runs), instead of the system clock.
- `vcpe_gfx_bench(n)`: n frames drawn synchronously even when pipelined, so `vcpe_stat_ge_us` is the GE's own time.
- Compile-time sizes: `-DVCPE_DLIST_KB` (display list, default 512), `-DVCPE_AUDIO_FRAMES` (mixer block, default
  1024 = 23 ms; 512 halves the latency), `-DVCPE_TILES_CAP` / `-DVCPE_TILES_LAYERS` (vcpe_tiles BSS), `-DVCPE_CO_POOL`.

Measuring (the same numbers in every game):
- `vcpe_stat_vblanks` / `vcpe_stat_vblank_misses`: vblanks between frames shown, and every late frame since start.
  A silent 60 -> 30 fps drop on hardware shows up here.
- `vcpe_stat_ge_us`, `vcpe_stat_dlist_kb` / `vcpe_stat_dlist_peak_kb`. Without pipelining, `vcpe_stat_build_us` and
  `vcpe_stat_gpu_wait_us` are now filled too (they stayed 0, so the overlay's DRAW showed 0.0+0.0).
- `vcpe_hud_perf_detail(1)`: a second overlay line (GE ms, binds, overdraw in screens, late vblanks, display list).
- `vcpe_perf_line()` and `vcpe_mem_report()`: one standard perf line and one memory line (used, peak, largest free
  block, free holes, sprite cache with loads and failures, display list peak, failed reloads) for logs.
- `vcpe_mem_largest_free_kb()`: the largest free block, public.

Robustness:
- The display list can no longer overflow. `sceGuGetMemory` has no limit check and never returns 0 (checked in
  the pspsdk library), so a full list wrote past its end into the BSS behind it. Engine draws now take memory
  through `vcpe_gfx_mem()`, which drops the draw and counts it (`vcpe_stat_dlist_full`) when fewer than 16 KB would
  be left for state commands.
- `vcpe_bind_page` / `vcpe_page_size` check the page index (the table length comes from the bundle list).
- `vcpe_spr_init` frees its tables when one fails, so later draws are no-ops instead of reading freed or half-made
  tables. Failed page loads are counted (`vcpe_spr_fails`).
- `vcpe_test_load` checks its allocation.
- Fix: `vcpe_bundle_load_at` marked the resume as handled for every bundle, so loading a block after a sleep and
  before the next frame skipped reloading the volatile bundles.

## 3.4 (2026-10-15): the PSP-1000 (32 MB) and the loading screen

A game made for the 64 MB models can now fit a PSP-1000, which leaves about 20-21 MB of heap to a homebrew game.
docs/psp1000.md explains how a game adopts it.

- New module `vcpe_mem`:
  - `vcpe_mem_32mb()` tells a PSP-1000 from a PSP-2000/3000/Street/Go (measured once, early).
  - `vcpe_volatile_mem()` locks the 4 MB volatile partition at 0x08400000 for the game (every model has it;
    commercial games use it too).
  - `vcpe_mem_used_kb` / `vcpe_mem_peak_kb` for memory audits.
- `vcpe_gfx`, bundles in memory the game chooses:
  - `vcpe_bundle_slab(first, last)`: those bundles take turns in one fixed block the size of the largest. After an
    8 MB level, a 9 MB level still fits, because the heap can no longer fragment around the level textures.
  - `vcpe_bundle_load_at(b, mem, cap, reload)`: a bundle read into given memory, such as the volatile partition.
    With reload 1 it is read again after every sleep/resume, because it is not verified that the partition keeps
    its contents while the console sleeps.
- New module `vcpe_load`: the universal loading screen. Between `vcpe_loading_begin(bytes)` and
  `vcpe_loading_end()`, the game unloads the old content and loads the next while the PSP shows "LOADING" and a
  progress bar, instead of a frozen picture.
  - PAK reads report their progress themselves: they are read in 256 KB chunks while a loading screen is up.
  - The standard screen is the same in every game, like the error screen: black, with these parts:
    - a boot log in the top-left corner, written automatically, in plain text with one line per step
      (`Loading textures 14 to block: 9060 KB`). The OK/FAILED results only go to stdout in autotest runs
    - the engine's loading animation in the bottom-right corner: `engineassets/wow.gif`, 24 frames at 96x96 in one
      256-colour T8 palette (about 217 KB), built into `src/vcpe_loadimg.h` by
      `tools/mkloadimg.py [GIF] [SIZE] [STEP]`. Only games that link `vcpe_load` carry it
    - "LOADING..." and the progress bar just above the animation
  - Steps (`vcpe_sys`: `vcpe_step_begin/end/note`): engine modules report what they do, and the boot log shows it.
    The reports cover PAK reads, texture bundles (load, unload, the slab block, volatile memory, reloads after
    sleep), the volatile-memory lock, sound effects, music and audio start. Games add their own steps; the 2D runtime reports scene unload, world load, object creation, script linking and text. A step inside another step
    is not reported. In autotest runs, finished lines also go to stdout. With no loading screen up, a step costs
    one test.
  - The screen appears only when a load takes longer than 150 ms (`vcpe_loading_show_after`).
  - `vcpe_loading_draw_hook` gives a game its own screen, which can still use `vcpe_loading_anim` (the animation
    anywhere) or `vcpe_loading_draw_standard`. `vcpe_loading_text` changes or translates the word.
  - Loading frames are not autotest frames, so recordings stay frame-exact.
- `vcpe_gfx_in_frame()`: true between `vcpe_gfx_begin` and `vcpe_gfx_end`.
- **Fix:** `vcpe_heap_kb()` always reported about 64 MB. GCC removed the probe's `malloc` that was only `free`d
  and treated it as a success. As a result, `vcpe_require_64mb` never refused a PSP-1000, and games that call it
  ran on one until they ran out of memory. They now show the "needs 64 MB" message as intended.
- Self-test: covers the model, the volatile partition, slab turns, placed bundles and the loading screen (with a
  snapshot of it).
- Games that do not link `vcpe_mem`/`vcpe_load` or call the new functions are unchanged. The only exception is the
  `vcpe_heap_kb` fix.

## 3.3 (2026-10-15): the mouse on the PSP

- New module `vcpe_mouse`: a PC mouse simulated with the PSP controls, for ports of games made for a pointer (drag
  and drop inventories, point-and-click, card and board games, menus).
  - Analog stick moves the pointer. It is time-based, with a deadzone, a response curve, a boost at the rim, and a
    "slow" button for precise aiming.
  - D-pad warps the pointer to the nearest point of interest in that direction. Targets must lie within 50 degrees
    and are scored by distance times angle. Holding the d-pad repeats after 20 frames, then every 10 frames. The game
    lists its targets in a callback that runs only when a warp is asked for.
  - Left, right and middle buttons map to any PSP buttons, plus wheel up/down with repeat.
  - Press, release, click, double-click, drag start, drop and wheel edges are latched until read, like
    `vcpe_pad_pressed`. `vcpe_mouse_cancel_drag` handles drags the game refuses.
  - Hover helpers: `vcpe_mouse_in`, `vcpe_mouse_idle` (tooltip delays), and optional hide-when-idle.
  - Built-in pointers drawn from shapes (arrow, hand, grab, cross) at any integer scale, for games without cursor art.
  - Autotest commands: `mouse`, `glide`, `click`, `rclick`, `dclick`, `mdown`/`mup`, `drag`, `warp`, `wheel`,
    `mpos`. Chain `vcpe_mouse_command` before the game's commands and use `vcpe_mouse_busy` as the busy hook.
  - The defaults copy a shipped console port's controller cursor: 600 px/s on a 1920-wide screen (150 px/s at
    480), x1.5 at the rim, deadzone 0.2.
- Self-test (tests/selftest) covers clicks, double clicks, right clicks, drag and drop, warps, the wheel and the
  screen bounds.
- Games that do not link `vcpe_mouse` are unchanged. `vcpe.h` declares it and the version is now 3.3; no game draws
  the version string.

## 3.2 (2026-10-15): text rendered on the PSP

- New module `vcpe_ttf`: the PSP rasterizes TrueType/OpenType (glyf) fonts at runtime with stb_truetype (public
  domain, shipped with the pspdev toolchain), so text no longer has to be baked into pictures on the PC.
  - No hinting: at 12-17 px, FreeType's hinting on the PC snaps letters to whole pixels and spaces them unevenly
    ("absolut e"). Unhinted outlines keep the font's own spacing.
  - Pen positions in quarter pixels (each glyph is rasterized at the sub-pixel offset it lands on), kerning from
    GPOS/kern, any string at any pixel size, translations included.
  - Glyph cache: two 512x512 T8 pages (white with an alpha CLUT), drawn as tinted quads through `vcpe_gfx` (batched
    when batching is on). A page is recycled only when the frame being built has not drawn from it. New glyphs are
    written back from the data cache and the texture cache is flushed before the line's quads, which makes it safe
    with pipelining.
  - Ascent is rounded up like FreeType's, so layouts made with Pillow metrics keep their baselines.
  - No GSUB: ligatures (fi, fl) are not formed.
- `vcpe_gfx_frame`: counts `vcpe_gfx_begin` calls (for caches that must not change memory the current frame uses).
- Games that do not link `vcpe_ttf` are unchanged (the frame counter changes no output).

## 3.1 (2026-10-15): optimization and built-ins

Checked against every game built on VCPE: built with 3.1 and the new features left off, every recorded scenario
draws frame-identical to 3.0. The only exception is a scenario whose Lua scripts seed from the clock (see
`vcpe_lua_fixed_clock`).

### Faster, same output (on for everyone)
- `vcpe_audio`: the mixer now mixes one voice at a time over the 1024-sample block into an accumulator, instead of
  visiting all 24-32 voices and re-reading their volatile flags for every sample. The PCM is bit-identical to 3.0
  (host test `tests/host/audio_equiv.c`: 400 blocks of random starts, stops, pitch, gain and pause changes). In
  the emulator, the time it frees shows up as more frames in the same emulated time: a sprite-heavy test scenario
  draws 649 frames where 3.0 drew 609.
- `vcpe_spr`: sprite-font glyphs for ASCII come from a direct table instead of a binary search per character.

### Opt-in speed-ups (off by default, `vcpe_gfx.h`)
- `vcpe_gfx_batch(1)`: consecutive `vcpe_quad` calls and consecutive `vcpe_fill` calls go to the GE as one
  draw each. With batching forced on, games that draw only through the engine draw identically.
  Games that bind textures with raw `sceGu*` calls between engine draws must call `vcpe_gfx_flush()` before them
  first (docs/upgrading-to-3.1.md).
- `vcpe_gfx_pipeline(1)`: `vcpe_gfx_end` hands the frame to the GE and returns, so the next update runs while the
  GE draws; `vcpe_gfx_begin` waits for it, then the vblank, then swaps. With pipelining forced on, every game
  draws frame-identically (autotest presents before the script reads input). New calls:
  `vcpe_gfx_flush`, `vcpe_gfx_gpu_wait`, `vcpe_gfx_present`, `vcpe_gfx_pending`.
- New stats: `vcpe_stat_build_us`, `vcpe_stat_gpu_wait_us`, `vcpe_stat_draws`, `vcpe_stat_fps`.

### New built-in modules
- `vcpe_math`:
  - sin/cos from a table (degrees, radians, turns; error < 2e-6)
  - polynomial atan2 (< 1.2e-5 rad) and GameMaker-style `vcpe_direction_deg`
  - `sqrt.s` square root
  - clamp/lerp/approach/angle difference
  - xorshift RNG (`VcpeRng`, shared `vcpe_rng`), which picks ranges by multiply instead of modulo
  - `vcpe_sys_init` seeds it from the clock when the module is linked
- `vcpe_pad`:
  - a bind table from PSP buttons to game bits
  - the stick as the d-pad: plain threshold, or with hysteresis
  - reads the autotest script instead of the pad when there is one
  - press edges that survive frames without an update, released bits, anti-mash debounce, and menu auto-repeat
  - `vcpe_pad_button` maps script names, including "a+b" chords
- `vcpe_save`: two-slot saves next to the EBOOT with version, size, sequence number and CRC-32. A torn write
  loses only that one save, and a file of another version or size reads as "no save".
- `vcpe_log`: a log next to the EBOOT, line-flushed or buffered. `vcpe_fatal` writes its message to it.
- `vcpe_hud`:
  - text in the PSP SDK's 8x8 font
  - the standard perf overlay: FPS, update ms, draw ms (CPU build + GE wait), calls, quads, free memory
  - PSP button icons drawn from shapes in the PlayStation colours, and prompts like "(X) JUMP"
- Start-up checks with player-facing messages: `vcpe_require_64mb`, `vcpe_require_file`, and
  `vcpe_pak_open_checked`, which compares the PAK header {magic, size, stamp} with the build. It reports a
  missing PAK, a PAK cut short, and a PAK from another build.
- `vcpe_audio`: `vcpe_sfx_play(id, pitch, gain, first, last)` picks a free voice in a range, or takes the one
  started longest ago.
- `vcpe_test`: `snap NAME` built in when the game has no snap command, and `vcpe_test_snap()`.
- `vcpe_lua`: `vcpe_lua_fixed_clock(vm, t)` makes scripts that seed from `os.time()`/`os.clock()` repeatable.
- `tools/vcpe_pak.py`: PAK writer for converters (the header `vcpe_pak_open_checked` reads, plus the C header with
  `PAK_SIZE`, `PAK_STAMP` and section offsets).

### Tests
- A self-test EBOOT that exercises every built-in. It checks the PAK, math, saves, audio, pad
  edges, and that batched and pipelined snapshots equal the plain one.
- Host tests for mixer equivalence, math accuracy and save recovery (plain gcc, no PSP).
