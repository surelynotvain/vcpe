# VCPE 5.5: Vain's C Portable Engine

A game engine in plain C for the **Sony PlayStation Portable**, built for demakes: PC games rewritten to run on the
PSP's 333 MHz CPU, its GE graphics chip and its 32 or 64 MB of memory.

VCPE has no splash screen and no scripting requirement. A game links only the modules it uses.

**Current release: [VCPE 5.5](https://github.com/surelynotvain/vcpe/releases/tag/vcpe-5.5).**
5.5 adds an open-world toolkit for large 3D games: compressed skeletal animation and a layered animator with foot
IK, streamed heightfield terrain, open-world cell streaming with a floating origin, static 3D collision with a
capsule character controller, a day cycle with blended weather and rain/snow, and level of detail for crowds of
characters. It includes everything from 5.1 (3D skinning, materials, display-list guards, paths, vehicles, backdrops,
profiling) and 5.0 (reversible upgrades, native savedata, camera culling, the standard loading screen). Every new
module is opt-in; existing games build unchanged and only the loading credit now reads “made with VCPE 5.5”.

The [5.5 upgrade guide](docs/upgrading-to-5.5.md) lists the new modules, their dependencies and PSP-1000 limits.
Earlier guides: [5.1](docs/upgrading-to-5.1.md), [5.0](docs/upgrading-to-5.0.md) (integration and rollback). Read the
[native savedata guide](docs/savedata.md) and [camera-culling guide](docs/camera-culling.md) for their ownership and
invalidation rules. The [Blank Engine](docs/blank-engine.md) builds a standalone demo with no game files.

## Versions

| Version | Source | What it added |
|---|---|---|
| **1.0** | [Release](https://github.com/surelynotvain/vcpe/releases/tag/vcpe-1.0) | System callbacks, data PAK, GU renderer, ADPCM mixer, autotest harness, converter tools |
| **2.0** | [Release](https://github.com/surelynotvain/vcpe/releases/tag/vcpe-2.0) | A Unity-style 2D runtime: scenes and prefabs, animator, 2D physics subset, pathfinding, particles, glyph text |
| **3.0** | [Release](https://github.com/surelynotvain/vcpe/releases/tag/vcpe-3.0) | 3D module (depth buffer, camera, indexed meshes, fog), node tree with animation players, coroutines, baked fonts, view culling, post effects without shaders, Godot 4 scene tools, start-up error screen |
| **3.1** | in 3.3 | Faster mixer (bit-identical output), opt-in draw batching and GE pipelining, math tables, pad input, crash-safe saves, log file, HUD text and PSP button icons, perf overlay, start-up checks, self-test EBOOT |
| **3.2** | in 3.3 | `vcpe_ttf`: TrueType/OpenType text rasterized on the PSP, sub-pixel pen positions, kerning, glyph cache |
| **3.3** | [Release](https://github.com/surelynotvain/vcpe/releases/tag/vcpe-3.3) | `vcpe_mouse`: a PC mouse simulated with the analog stick and buttons (warps, drag and drop, wheel) |
| **3.4** | in 3.5 | `vcpe_mem`: PSP-1000 (32 MB) support, the 4 MB volatile partition, texture slabs that can't fragment; `vcpe_load`: the standard loading screen with a boot log |
| **3.5** | [Release](https://github.com/surelynotvain/vcpe/releases/tag/vcpe-3.5) | `vcpe_data`: read-only structured data from converters; `vcpe_tiles`: visible-tile batching for 2D maps |
| **4.0** | [Release](https://github.com/surelynotvain/vcpe/releases/tag/vcpe-4.0) | Text binds its texture once instead of once per string, O(1) sprite cache, pooled coroutine threads, sleep/resume reload behind the loading screen, display-list overflow guard, arenas, vblank and GE timing, one standard perf and memory line |
| **4.1** | in 4.7 | Event bus, blackboard and live tuning; priority reservation workers; compact asset packs; mesh LOD and offline tools; standalone Blank Engine |
| **4.5** | in 4.7 | Variable-size checked saves and optional multichannel ADPCM streams with intro/loop points, pitch, fades and resumable positions |
| **4.7** | [Release](https://github.com/surelynotvain/vcpe/releases/tag/vcpe-4.7) | Memory budgets and automatic cosmetic scaling; cached transforms and draw ordering; deterministic collision broadphase; demand-grown effects/components and stable stores; lazy font atlas; predictive loading; frame and allocation diagnostics |
| **5.0** | [Release](https://github.com/surelynotvain/vcpe/releases/tag/vcpe-5.0) | Reversible per-project engine upgrades; native savedata and XMB metadata; automatic batch/chunk camera culling with mutable bounds; Czechia loading animation, version credit and eight-row status log |
| **5.1** | [Release](https://github.com/surelynotvain/vcpe/releases/tag/vcpe-5.1) | `vcpe_skin` four-weight CPU/VFPU skinning; 3D filtering and per-batch materials; display-list headroom checks; `vcpe_path` splines, `vcpe_vehicle` towed wagons, `vcpe_backdrop` sky dome and distant ridges, `vcpe_profile` frame windows, `vcpe_canvas`, `vcpe_offline`; exact ICON1 timestamps; experimental flat perspective, RGSS runtime and asset cook pipeline |
| **5.5** | [Release](https://github.com/surelynotvain/vcpe/releases/tag/vcpe-5.5), `main` | Open-world toolkit: `vcpe_rig` compressed clips, IK and skinned-mesh LOD (glTF converter); `vcpe_animator` layers, blend spaces and foot IK; `vcpe_terrain` streamed heightfield cells; `vcpe_cells` predictive streaming and floating origin; `vcpe_collide` meshes, rays and capsule controller; `vcpe_sky` day cycle, weather and rain/snow; `vcpe_crowd` character LOD |

3.1, 3.2 and 3.4 have no snapshot of their own: they were finished on the way to the next version, and their source
is part of 3.3 and 3.5. The 4.1 and 4.5 development iterations are included in 4.7 without separate release tags.
[CHANGELOG.md](CHANGELOG.md) describes the releases, and [docs/upgrading-to-5.5.md](docs/upgrading-to-5.5.md)
says what a game can adopt.

There is no published roadmap. Features are added when the engine needs them, and are listed here once they are
released.

## Build and check

Build the standalone engine with Podman and the [pspdev](https://github.com/pspdev/pspdev) toolchain image:

```sh
sh blank/build.sh
```

Copy `blank/EBOOT.PBP` to `PSP/GAME/VCPE55/`. Left/right changes pages: SYSTEM, JOBS, ULTRA, PERF, ADAPT,
BENCH and SAVES. On SAVES, Cross saves, Circle loads and Triangle previews the loader. HOME exits.
The demo enables adaptation so its behavior can be inspected; games opt in separately.

Upgrade an older project with a manifest-checked engine snapshot:

```sh
python3 tools/vcpe_upgrade.py plan /path/to/demake
python3 tools/vcpe_upgrade.py apply /path/to/demake
```

The upgrade tool also resolves the dependencies of the 5.1 and 5.5 modules a game adds to `VCPE_MODULES`.

Add `--savedata` to resolve the optional native savedata modules and utility library. Games must integrate
the save calls and deliberately import old saves; upgrading does not rewrite gameplay or save schemas.

Run the host suites with a C compiler, Python 3, Pillow, NumPy, Fontconfig and a system TrueType font:

```sh
sh tests/run.sh
```

`tests/run.sh` includes `tests/v52/run.sh`, the open-world suites (rig, animator, terrain, cells, collide, sky,
crowd; 1125 checks under ASan/UBSan).

The font-cache test reads `stb_truetype.h` from the pspdev container unless `VCPE_STB_INCLUDE` points to a local
copy. The tests use temporary synthetic data. Set `VCPE_AUDIO_BASELINE` to an older `vcpe_audio.c` to compare
400 mixer output blocks byte for byte. To run the standalone PSP self-test:

```sh
PSP_EMU=/path/to/PPSSPPHeadless sh tests/selftest/run.sh
```

## How it is tested
- **Frame-exact recordings.** Autotest scripts drive a build in PPSSPP headless with a fixed time step and seed. Every
  regression run can build several engine clients and compare their output with the previous version.
  A change that affects output needs an explanation or an opt-in switch.
- **Self-test EBOOT.** A standalone EBOOT exercises the PAK, math, saves, audio, input, mouse,
  memory, loading screen, coroutines and the 4.0 binding cache. It also checks that batched and pipelined
  snapshots match the plain one.
- **Host tests.** Runtime state/contact hashes, draw ordering and conservative bounds, font eviction and allocation
  failure, real-thread loading, adaptive pressure handling, memory, math, saves, input and converter checks.
- **4.7 verification.** Host suites and standalone PPSSPP checks pass. The controlled collision fixture reduces
  candidates from 3,941,120 to 614,650 with identical state/contact hashes. These numbers describe that fixture,
  not a guaranteed speedup in every game. See the [release checks](docs/upgrading-to-4.7.md#verification).
- **5.0 verification.** Upgrade/rollback, volatile-memory handoff and culling CPU contracts pass. Culling checks
  include 20,000 camera/affine cases, mocked GU submission, dynamic bounds and allocation failures, including
  Clang ASan/UBSan and fast-math builds. Earlier standalone 5.0 checks cover savedata round trips/imports,
  metadata/icons and animated loader captures. New culling has build/CPU evidence; complete-game visual and
  frame-time acceptance remain pending. See [5.0 release verification](docs/release-5.0.md).
- **5.1 verification.** Skinning, culling, vehicle, perspective, capture, memory and ICON1 suites pass under Clang
  with sanitizers where enabled; the new modules compile with `psp-gcc -Wall -Wextra` without warnings. See
  [5.1 release verification](docs/release-5.1.md).
- **5.5 verification.** The open-world host suites pass (1125 checks, ASan/UBSan): rays against analytic hits, the
  capsule through walls, stairs, ramps, ledges and long falls, streaming with a fake async loader and real packs,
  seamless terrain cells, frame-exact precipitation. Every new module compiles with psp-gcc 15 `-Wall -Wextra`
  without warnings. They have not yet run in an emulator or on a PSP; time them on a PSP-1000 before relying on them.
- **Hardware.** This release has no new physical PSP verification. Emulator results do not establish GE texture-cache,
  real memory-stick, sleep/resume or audio-timing behavior. Test those on each supported console before release.

## Layout

| Folder | What it is |
|---|---|
| [`src/`](src/) | Engine modules (`vcpe_*.c/.h`); `vcpe.h` gathers common APIs, with separate headers for optional modules |
| [`runtime/`](runtime/) | The 2D runtime from 2.0 |
| [`rpg/`](rpg/), [`cook/`](cook/) | Experimental RGSS-style runtime and PC-side asset cooking |
| [`tools/`](tools/) | Converters: glTF rigs and clips, terrain heightmaps, texture pages, ADPCM sound, PAK writer, fonts, Godot 4 scene helpers, XMB icon/background/sound makers |
| [`blank/`](blank/) | Standalone engine UI, diagnostics and demonstrations |
| [`docs/`](docs/) | Upgrading/rollback, open-world modules, 3D skinning, flat perspective, native savedata, camera culling, automatic budgets, memory, jobs/packs, LOD, PSP-1000, input and converters |
| [`engineassets/`](engineassets/) | Error/loading/Blank Engine pictures (third-party, see the license); generated image data retains those owners' terms |
| [`tests/`](tests/) | Host suites (including the 5.5 open-world suites in `v52/`) and a portable standalone PSP self-test |
| [`LICENSE`](LICENSE), [`AI_POLICY.md`](AI_POLICY.md), [`CONTRIBUTING.md`](CONTRIBUTING.md), [`CHANGELOG.md`](CHANGELOG.md) | Terms, how AI is used, how to contribute, what changed |

A game's Makefile compiles the modules it lists from `src/` (and `runtime/` if it uses it) next to its own code,
with the [pspdev](https://github.com/pspdev/pspdev) toolchain.

## AI use and human contributions

VCPE is developed with AI assistance, and that is disclosed rather than hidden. The full policy is in
[AI_POLICY.md](AI_POLICY.md). In short:

- **What AI does.** AI coding assistants have written and still write a large share of the code, tools, tests and
  documentation, and are used to review changes, analyse performance and search for bugs.
- **What people do.** The maintainer sets the direction, decides what is accepted, runs the hardware tests and is
  responsible for every release. AI tools are not authors or contributors of record: commits and releases are made
  by people.
- **What is checked.** No change is kept on an AI's word. Every engine change has to keep the frame-exact recordings
  identical or explain the difference, pass the self-test and host tests, and anything an emulator can't show
  (audio, sleep, texture cache, real timing) is checked on a PSP before it becomes a default.
- **What can still be wrong.** Tested is not proven: AI-written code can be subtly wrong in paths no test covers.
  Reports of that kind are especially valuable.

**Human contributions are more than welcome**, and they are what this project needs most: hardware test results
(PSP-1000, PSP Go and the Street especially), bug reports with a reproduction, fixes, performance measurements,
documentation and reviews of existing code. Contributions written with AI help are accepted too, under the rules in
[CONTRIBUTING.md](CONTRIBUTING.md): say so, understand what you submit, and test it.

## License

| Versions | License |
|---|---|
| 3.0 and later | [VCPE Contribute-Back License 1.1](LICENSE) |
| 1.0 and 2.0 | MIT (as released) |

What it means in practice (the [LICENSE](LICENSE) text is what counts):

- **Free to use** in any project, commercial or not, including closed-source games.
- **Engine changes go back upstream.** If you distribute anything containing modified VCPE files, you send those
  modifications to this repository as a pull request within 30 days. Changes you never distribute (private
  experiments, internal builds) carry no obligation.
- **Your project stays yours.** A game's own code, art, audio and data are never covered, however they are linked
  with VCPE.
- **Credit VCPE** in your project's credits or readme.
- **Patents:** contributors grant a patent license for their contributions; it ends for anyone who sues over them.
- **Third-party material** (the error-screen and loading pictures in `engineassets/`, toolchain libraries) keeps
  its own terms.

Releases 3.0 to 4.0 shipped with version 1.0 of this license; anyone may choose to use them under 1.1 instead.
