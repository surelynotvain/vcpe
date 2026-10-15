# Upgrading to VCPE 4.0

A game built on 3.x builds on 4.0 unchanged and draws the same frames (verified with the engine's frame-recording
regression). Nothing has to change. This page lists what a game can adopt, and what the 4.0 review found.

## Rules that are new
- A game that binds textures with raw `sceGu*` calls must call `vcpe_unbind()` afterwards: the engine's own page
  cache needs it, and the HUD, TTF and loading screen now rely on it too (they skip re-binding when their texture
  is still bound). Every game that binds raw textures
  already did.
- A game that draws with raw `sceGuGetMemory` can use `vcpe_gfx_mem()` instead, which returns 0 rather than
  overflowing the display list.

## Worth adopting
| What | When | How |
| --- | --- | --- |
| `vcpe_opaque(1)` | tilemaps, full-screen backgrounds | around the opaque draws, `vcpe_opaque(0)` after |
| `vcpe_spr_arena(0, budget)` | sprite games on the PSP-1000 | after `vcpe_spr_init`, instead of malloc per page |
| `vcpe_arena_*` | per-level tables and maps | `vcpe_arena_reset` at each scene switch |
| `vcpe_ttf_prewarm` | TTF dialog scenes | behind the loading screen, with that scene's characters |
| `vcpe_dt()` | animation timed with `sceKernelGetSystemTimeWide` | use the vblank-based dt instead |
| `vcpe_mem_report` + `vcpe_perf_line` | test builds | print both at every scene switch |
| `vcpe_hud_perf_detail(1)` | hardware testing | with the overlay; watch LATE and BINDS |
| `vcpe_gfx_lazy_texflush(1)` | busy scenes, after a hardware check | compare on a real PSP before shipping |

## The review, point by point
An external review of 3.5 raised 20 points. Each was checked against the code; some were wrong or did not apply.

| # | Point | Result |
| --- | --- | --- |
| 1 | HUD/loading text re-binds per string | Done: texture identity (`vcpe_tex_claim`/`vcpe_tex_bound`), also for TTF pages |
| 2 | `sceGuTexFlush` on every bind | Opt-in `vcpe_gfx_lazy_texflush(1)`: PPSSPP ignores the GE texture cache, so only hardware can verify it |
| 3 | Opaque state, GE-only timing, draw sorting | `vcpe_opaque`, `vcpe_gfx_bench` + `vcpe_stat_ge_us`. Sorting: `vcpe_tiles` with `disjoint=1` already groups by image |
| 4 | First-use hitches | sin table at init, `vcpe_ttf_prewarm`, row-only page clear. Advance caching skipped: `stbtt_GetGlyphHMetrics` is two table reads |
| 5 | vblank misses, dt | `vcpe_stat_vblank_misses`, `vcpe_stat_vblanks`, `vcpe_dt()` |
| 6 | Reload hitch after sleep | Behind the loading screen, plus `vcpe_resume_pending`/`vcpe_bundles_reload` |
| 7 | Mixer at the main thread's priority | Wrong: the main thread runs at 0x20 (pspsdk default, no game changes it), the mixer at 0x12 is already above it. Block size is now `-DVCPE_AUDIO_FRAMES` |
| 8 | Audio ring "384 KB" | Wrong: the ring is 48 x 1040 bytes = 49 KB (see the table below) |
| 9 | Coroutine thread churn | Done: parked threads reused (`VCPE_CO_POOL`) |
| 10 | Sprite page cache churn | O(1) LRU by default; fixed-slot arena opt-in (allocating the whole budget up front would take memory that games with a generous budget never used) |
| 11 | `load_all` needs one block | Falls back to one allocation per page |
| 12 | Arena, memory report | `vcpe_arena_*`, `vcpe_mem_report` |
| 13 | Fragmentation visibility | `vcpe_mem_largest_free_kb`, "holes" in the report |
| 14 | Slab freed and allocated again | Kept when it fits |
| 15 | BSS | Sizes measured below; `VCPE_DLIST_KB`, `VCPE_TILES_CAP`/`LAYERS` configurable. TTF pages are heap, not BSS |
| 16 | Seek per read, 33 ms redraws, flush per eviction | Seek skipped when in place, 50 ms default, one GE wait per eviction pass. The `sceGuSync` in `vcpe_bundle_unload` stays: inside a frame (GU_DIRECT) the GE may still be drawing the current list |
| 17 | Unchecked `sceGuGetMemory` | A NULL check would never fire (the library has no limit check). Fixed with `vcpe_gfx_mem` and a fill counter |
| 18 | Bounds, leaks, silent failures | Page index bounds, `vcpe_spr_init` cleanup, failure counters, `vcpe_test_load` check |
| 19 | `-O3 -flto` | Not done: the flags live in each game's Makefile, and LTO with `-G0` is untested on the PSP. Try per game and compare recordings |
| 20 | One standard perf line | `vcpe_perf_line` |

## Memory per module (4.0, measured with psp-size)
BSS and large constant data a game pays for when it links the module:

| Module | BSS | Also |
| --- | --- | --- |
| vcpe_gfx | 553 KB: display list 512 KB (`VCPE_DLIST_KB`), batch arrays 40 KB | bundles on the heap |
| vcpe_tiles | 464 KB (`VCPE_TILES_CAP` x `VCPE_TILES_LAYERS`) | |
| vcpe_ttf | 183 KB (glyph and kerning tables) | 512 KB heap for the two glyph pages, at the first draw |
| vcpe_load | 1 KB | 217 KB animation in the EBOOT |
| vcpe_audio | 63 KB (music ring 49 KB, output blocks) | sound effects on the heap; threads 16 + 32 KB stack |
| vcpe_math | 16 KB (sin table) | |
| vcpe_hud | 8 KB (font) | |
| vcpe_spr | 4 KB | tables + page cache on the heap (budget) |
| vcpe_co | 2 KB | 32 KB stack per running or parked coroutine |

`vcpe_stat_dlist_peak_kb` shows how much of the display list a game really uses. Measure it in the busiest scene
before trimming `VCPE_DLIST_KB` on a PSP-1000; a full list now drops draws (`vcpe_stat_dlist_full`) instead of
corrupting memory.
