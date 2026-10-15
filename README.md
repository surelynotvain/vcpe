# VCPE: Vain's C Portable Engine

A game engine in plain C for the **Sony PlayStation Portable**, built for demakes: PC games rewritten to run on the
PSP's 333 MHz CPU, its GE graphics chip and its 32 or 64 MB of memory.

VCPE has no splash screen and no scripting requirement. A game links only the modules it uses.

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
| **4.0** | [Release](https://github.com/surelynotvain/vcpe/releases/tag/vcpe-4.0), `main` | Text binds its texture once instead of once per string, O(1) sprite cache, pooled coroutine threads, sleep/resume reload behind the loading screen, display-list overflow guard, arenas, vblank and GE timing, one standard perf and memory line |

3.1, 3.2 and 3.4 have no snapshot of their own: they were finished on the way to the next version, and their source
is part of 3.3 and 3.5. [CHANGELOG.md](CHANGELOG.md) describes every version from 3.1 on, and
[docs/upgrading-to-4.0.md](docs/upgrading-to-4.0.md) says what a game can adopt.

There is no published roadmap. Features are added when the engine needs them, and are listed here once they are
released.

## How it is tested
- **Frame-exact recordings.** Autotest scripts drive a build in PPSSPP headless with a fixed time step and seed. Every
  engine change is built against several games and compared frame by frame with the previous version.
  Behaviour changes stay opt-in until they draw the same frames.
- **Self-test EBOOT.** One EBOOT exercises every built-in module: the PAK, math, saves, audio, input, mouse,
  memory, loading screen, coroutines and the 4.0 binding cache. It also checks that batched and pipelined
  snapshots match the plain one.
- **Host tests.** Plain-gcc tests for the mixer, math accuracy, saves, input edges and file converters.
- **Hardware.** Audio, sleep/resume and the GE texture cache can't be proven in an emulator, so they are checked on
  a PSP.

## Layout

| Folder | What it is |
|---|---|
| [`src/`](src/) | The engine modules (`vcpe_*.c/.h`); `vcpe.h` includes them all |
| [`runtime/`](runtime/) | The 2D runtime from 2.0 |
| [`tools/`](tools/) | Converters: texture pages, ADPCM sound, PAK writer, fonts, Godot 4 scene helpers, XMB icon/background/sound makers |
| [`docs/`](docs/) | Guides: upgrading, the PSP-1000, input and converters |
| [`engineassets/`](engineassets/) | Source pictures of the error screen and the loading animation (third-party, see the license) |
| [`tests/`](tests/) | Host tests |
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
