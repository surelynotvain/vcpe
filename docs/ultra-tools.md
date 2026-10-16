# Ultra tools (`vcpe_ultra`)

Fast communication between the engine and the game, and the helpers every game ends up writing. Link the module
(`vcpe_ultra` in `VCPE_MODULES`) and call one function per frame:

```c
vcpe_gfx_end();
vcpe_ultra_update();   /* engine events, tuning poll, profiler frame, then delivers every queued event */
```

## Event bus
```c
static void on_resume(const VcpeEvent *e, void *user) { music_restart(); }
vcpe_ev_on(VCPE_EV_RESUME, on_resume, 0);

enum { EV_PLAYER_DIED = VCPE_EV_GAME, EV_COIN };
vcpe_ev_post(EV_COIN, 10, 0, 0);          /* i = amount */
```
- Delivery happens in `vcpe_ev_dispatch` (called by `vcpe_ultra_update`), in posting order, on the main thread.
  A listener that posts an event sees it delivered at the next dispatch, never recursively.
- From another thread (audio, a loader thread) use `vcpe_ev_post_thread`. It is lock-free for one producer
  thread at a time.
- Queues are fixed (256 main, 64 thread). A full queue drops the event and counts it in `vcpe_ev_dropped`.
- Engine events: `VCPE_EV_RESUME`, `VCPE_EV_LOAD_BEGIN` / `END`, `VCPE_EV_LATE_FRAME` (a missed vblank),
  `VCPE_EV_LOW_MEMORY` (after `vcpe_ultra_low_memory_kb(kb)`), `VCPE_EV_BB_CHANGED`, `VCPE_EV_TUNE_CHANGED`.

## Blackboard
```c
vcpe_bb_set_int("player.hp", hp);         /* posts VCPE_EV_BB_CHANGED only when the value changes */
int hp = vcpe_bb_int("player.hp", 3);
```
Up to 256 names, values of up to 31 characters. Useful for state the engine, debug overlays and test scripts all
read (`bbcheck player.hp 3` in AUTOTEST.TXT logs PASS or FAIL).

## Live tuning
```c
vcpe_tune_float("player.speed", &speed, 0, 10);
vcpe_tune_int("enemy.count", &count, 1, 50);
vcpe_tune_file("host0:/tune.txt", 30);     /* PSPLink: edit on the PC; default is TUNE.TXT next to the EBOOT */
```
The file is `name = value` per line, `#` starts a comment. It is checked every 30 frames and re-read only when
its size or time changed. Values are clamped to their range, and every change posts `VCPE_EV_TUNE_CHANGED`.
`vcpe_tune_save()` writes the current values, a good starting file. Leave tuning out of release builds (don't
call `vcpe_tune_file`, or pass 0 frames).

## Toolkit
| Helper | Use |
|---|---|
| `vcpe_fnv(name)` | 32-bit FNV-1a, for ids from names |
| `vcpe_fx_*` | 16.16 fixed point: identical results everywhere (replays, lockstep) |
| `VcpeRing` | FIFO of fixed-size items over your memory, power-of-two capacity |
| `VcpePool` | fixed-size blocks, O(1) alloc/free, no fragmentation (bullets, particles) |
| `VcpeTimer` | countdowns that fire once or repeat without drifting |
| `vcpe_prof_*` | named scopes, microseconds per frame averaged over 30 frames; `vcpe_prof_line` for logs |

## Memory
About 33 KB of BSS (measured with psp-size) and 8 KB of code: blackboard 18 KB, tuning table 8 KB, event queues 5 KB,
listeners and profiler about 1 KB.
No heap allocation.
