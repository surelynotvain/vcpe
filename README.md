# VCPE: Vain's C Portable Engine

A small game engine in plain C for the **Sony PlayStation Portable**, made for demakes of PC games.

## Versions

| Version | What it is | Used by | Source |
|---|---|---|---|
| **VCPE 1.0** | The base: system, data PAK, GU renderer, ADPCM mixer, test harness, tools | [Helltaker PSP](https://github.com/surelynotvain/helltaker-psp) | [Release](https://github.com/surelynotvain/vcpe/releases/tag/vcpe-1.0) |
| **VCPE 2.0** | The base plus a Unity-style runtime (scenes, animator, 2D physics, pathfinding, particles, glyph text) | [Awaria PSP](https://github.com/surelynotvain/awaria-psp) | [Release](https://github.com/surelynotvain/vcpe/releases/tag/vcpe-2.0) |
| **VCPE 3** | In development, on `main` | | |

The source code of each finished version is in the **Source code** downloads of its release.

## VCPE 3

### Layout

| Folder | What it is |
|---|---|
| [`src/`](src/) | The base: `vcpe_sys`, `vcpe_pak`, `vcpe_gfx`, `vcpe_audio`, `vcpe_test` |
| [`runtime/`](runtime/) | The Unity-style runtime from VCPE 2: world and prefabs, animator, Physics2D subset, A* pathfinding, particles, sorted draw list, script layer, PlayerPrefs, sprites and glyph text |
| [`tools/`](tools/) | Converter helpers (texture pages, ADPCM) and the XMB file makers |

A game compiles `src/` and, if it uses the runtime, `runtime/` together with its own generated data header (the converter's tables).

### Cleanup so far

- The runtime now lives in the engine instead of inside the Awaria port
- Game rules are out of the runtime: a demo build's chapter limit is a game hook (`g_scene_filter`), and the runtime no longer includes the game's input header
- Both games still build from this tree and give byte-identical frames in the regression test

### Next

- Rename the runtime files to one naming scheme and replace the remaining game data names with a documented data format
- Then 3D: a GU 3D module (depth buffer, hardware lights, fog, skinned meshes), low-resolution render-to-texture for PS1-style looks, and a converter for Godot 4 scenes

After that, the plan is 3D: a GU 3D module (depth buffer, hardware lights, fog, skinned meshes), low-resolution render-to-texture for PS1-style looks, and a converter for Godot 4 scenes.

## License

[MIT](LICENSE), by surelynotvain.
