# Moving a game to VCPE 3.1

A game built on 3.0 builds on 3.1 unchanged and draws the same. Adopt the new parts one at a time, and run
your frame regression after each step. Optimizations must not change frames. Built-ins that replace a game's own
code may change them, so look at the changed frames before accepting.

## 1. Pipelining (the biggest CPU win)
- Call `vcpe_gfx_pipeline(1)` after `vcpe_gfx_init()`.
- The rule: between `vcpe_gfx_end` and the next `vcpe_gfx_begin`, the update must not change or free memory the GE
  reads for that frame:
  - vertex arrays passed by pointer (`vcpe_3d_draw` meshes the CPU rewrites)
  - palettes given to `vcpe_spr_clut_override`
  - the game's own textures
  If the update has to change one, call `vcpe_gfx_gpu_wait()` first. Bundles, `vcpe_spr` pages and
  `sceGuGetMemory` data are already handled.
- A game that reads the framebuffer itself (its own snap command) calls `vcpe_gfx_present()` first, or uses
  `vcpe_test_snap()`.

## 2. Batching
- Call `vcpe_gfx_batch(1)`.
- Before every raw `sceGuTex*`/`sceGuClut*`/`sceGuDrawArray`/`sceGuEnable` in the game, call `vcpe_gfx_flush()`.
  Without it, the batch is drawn later with the game's texture.
- Find them with `grep -n "sceGu[A-Z]" src/*.c`.
- Check with your frame regression: frames must stay identical.

## 3. Built-ins that replace game code
| Typical game code | 3.1 |
|---|---|
| reading the pad, button names for tests, stick thresholds, anti-mash | `vcpe_pad_init` + `vcpe_pad_update`; `vcpe_pad_button` as `VcpeTestHooks.button` |
| `SAVE.DAT` written with `fwrite` | `vcpe_save_write` / `vcpe_save_read`; read the old file once as a fallback so players keep their progress |
| `logmsg` + `fopen("*.LOG")` | `vcpe_log_open` / `vcpe_log` |
| own `frand` / sin tables / `atan2` | `vcpe_math`; a different table size changes results slightly, so A/B the gameplay |
| own PAK header checks | `vcpe_pak_open_checked` when the PAK uses the `vcpe_pak.py` header; keep the game's own check for older layouts |
| SELECT FPS overlays | `vcpe_perf_update_begin/end` + `vcpe_hud_perf()` |
| "snap N" test commands | delete them: the engine's `snap` takes over |
| Lua scripts seeding from `os.time()` | `vcpe_lua_fixed_clock(vm, 0)` when `vcpe_autotest`, so autotest runs are frame-exact |

## 4. Check
1. Record the game's autotest scenarios before and after (`rec N` gives REC.RAW) and compare them frame by frame.
2. On hardware: hold SELECT and compare the overlay's numbers before and after.
