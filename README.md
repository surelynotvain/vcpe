# VCPE: Vain's C Portable Engine

A game engine in plain C for the **Sony PlayStation Portable**, built for demakes: PC games rewritten to run on the
PSP's 333 MHz CPU, its GE graphics chip and its 32 or 64 MB of memory.

VCPE has no splash screen and no scripting requirement. A game links only the modules it uses.

## Progress map

```mermaid
flowchart LR
    V1["1.0<br/>base"] --> V2["2.0<br/>2D runtime"] --> V3["3.0<br/>3D + scene tools"]
    V3 --> V31["3.1<br/>speed + built-ins"] --> V32["3.2<br/>TrueType text"] --> V33["3.3<br/>mouse"]
    V33 --> V34["3.4<br/>PSP-1000 + loading"] --> V35["3.5<br/>data + tiles"] --> V40["4.0<br/>smoothness + memory"]
    V40 --> NEXT["next<br/>hardware checks, 3D"]
    classDef pub fill:#2d6a4f,color:#fff,stroke:#1b4332
    classDef local fill:#b08900,color:#fff,stroke:#7a5f00
    classDef plan fill:#555,color:#fff,stroke:#333
    class V1,V2,V3 pub
    class V31,V32,V33,V34,V35,V40 local
    class NEXT plan
```

Green: published in this repository. Yellow: finished and tested, not published here yet. Grey: planned.

| Version | Status | What it added |
|---|---|---|
| **1.0** | [Release](https://github.com/surelynotvain/vcpe/releases/tag/vcpe-1.0) | System callbacks, data PAK, GU renderer, ADPCM mixer, autotest harness, converter tools |
| **2.0** | [Release](https://github.com/surelynotvain/vcpe/releases/tag/vcpe-2.0) | A Unity-style 2D runtime: scenes and prefabs, animator, 2D physics subset, pathfinding, particles, glyph text |
| **3.0** | On `main` | 3D module (depth buffer, camera, indexed meshes, fog), node tree with animation players, coroutines, baked fonts, view culling, post effects without shaders, Godot 4 scene tools, start-up error screen |
| **3.1** | Not published yet | Faster mixer (bit-identical output), opt-in draw batching and GE pipelining, math tables, pad input, crash-safe saves, log file, HUD text and PSP button icons, perf overlay, start-up checks, self-test EBOOT |
| **3.2** | Not published yet | `vcpe_ttf`: TrueType/OpenType text rasterized on the PSP, sub-pixel pen positions, kerning, glyph cache |
| **3.3** | Not published yet | `vcpe_mouse`: a PC mouse simulated with the analog stick and buttons (warps, drag and drop, wheel) |
| **3.4** | Not published yet | `vcpe_mem`: PSP-1000 (32 MB) support, the 4 MB volatile partition, texture slabs that can't fragment; `vcpe_load`: the standard loading screen with a boot log |
| **3.5** | Not published yet | `vcpe_data`: read-only structured data from converters; `vcpe_tiles`: visible-tile batching for 2D maps |
| **4.0** | Not published yet | Text binds its texture once instead of once per string, O(1) sprite cache, pooled coroutine threads, sleep/resume reload behind the loading screen, display-list overflow guard, arenas, vblank and GE timing, one standard perf and memory line |

### Next
- Check the 4.0 opt-ins on real hardware: lazy texture-cache flushes, a 512-sample audio block, resume after sleep
- Publish 3.1 to 4.0 here
- 3D: skinned meshes, 16-bit vertex formats, loading parts of a scene behind a black screen
- One naming scheme for the runtime files, and a documented data format in place of game-specific names
- Try `-O3` and link-time optimization against the frame recordings

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
| [`docs/`](docs/) | Guides |
| [`tests/`](tests/) | Host tests |

A game's Makefile compiles the modules it lists from `src/` (and `runtime/` if it uses it) next to its own code,
with the [pspdev](https://github.com/pspdev/pspdev) toolchain.

## Made with AI, open to people
AI tools were used in the development of VCPE and still are, for writing code, reviewing it and testing it. Every
change is checked against the frame recordings, and what an emulator can't show is checked on a PSP.

**Human contributions are more than welcome:** bug reports, hardware test results (especially PSP-1000 and PSP Go),
fixes, optimizations and docs. Open an issue or a pull request.

## License
[MIT](LICENSE), by surelynotvain.
