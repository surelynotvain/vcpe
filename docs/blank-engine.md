# Blank Engine

The engine built with no game: `blank/build.sh` makes `blank/EBOOT.PBP` (no game data files). Copy it
to `PSP/GAME/VCPE50/` and it boots into the Blank Engine UI over the engine picture
(`engineassets/staring.jpg`; `tools/mkblankimg.py` bakes it into `blank/src/blankimg.h` and `blank/ICON0.PNG`).

Pages (d-pad left/right, HOME to quit):
- **SYSTEM**: engine version and build time, console model (32/64 MB), firmware, CPU/bus clock, battery, heap,
  volatile partition, frame rate and late vblanks, uptime, linked modules.
- **JOBS**: the reservation demo. Press X to book 3 reservations at once for 2 clients (ai 70, loader 30), served
  by 2 servers; each one's priority, state and server is shown live.
- **ULTRA**: the event bus as events arrive, and the blackboard (X posts an event, UP raises a counter).
- **PERF**: the engine's perf overlay with the detail line.
- **ADAPT**: automatic cosmetic/LOD budgets, trigger, memory reclaimed and micro-work time/overruns. X toggles;
  Up/Down simulate overload/recovery; Triangle reclaims a real 64 KiB demo cache at loading. See
  [adaptive-budgets.md](adaptive-budgets.md). Automatic adaptation is enabled in this engine demo.
- **BENCH**: frame percentiles, CPU/GPU-wait/I/O diagnostics, allocation failures and an engine microbenchmark.
  Cross runs the benchmark.
- **SAVES**: native PSP savedata. Cross saves, Circle loads, Triangle previews the standard loading screen.
  Inspect the resulting entry in the XMB Saved Data Utility; see [savedata.md](savedata.md).

Uses: check a toolchain or an engine change without any game; check a console (model, firmware, battery,
memory); show the engine. It runs AUTOTEST.TXT scripts like a game, including the ultra tools' commands.
