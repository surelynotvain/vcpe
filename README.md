# VCPE: Vain's C Portable Engine

A small game engine in plain C for the **Sony PlayStation Portable**, made for demakes of PC games.

## Versions

| Version | What it is | Used by | Source |
|---|---|---|---|
| **VCPE 1.0** | The base: system, data PAK, GU renderer, ADPCM mixer, test harness, tools | [Helltaker PSP](https://github.com/surelynotvain/helltaker-psp) | [Release](https://github.com/surelynotvain/vcpe/releases/tag/vcpe-1.0) |
| **VCPE 2.0** | The base plus a Unity-style runtime (scenes, animator, 2D physics, pathfinding, particles, glyph text) | [Awaria PSP](https://github.com/surelynotvain/awaria-psp) | [Release](https://github.com/surelynotvain/vcpe/releases/tag/vcpe-2.0) |
| **VCPE 3** | In development, in this branch | | |

The source code of each finished version is in the **Source code** downloads of its release.

## VCPE 3

VCPE 3 starts with a cleanup of VCPE 2:

- the advanced layer moves into the engine and stops depending on one game's generated headers
- one layout and naming scheme for every module
- the base, the runtime and the tools documented together

After that, the plan is 3D: a GU 3D module (depth buffer, hardware lights, fog, skinned meshes), low-resolution render-to-texture for PS1-style looks, and a converter for Godot 4 scenes.

## License

[MIT](LICENSE), by surelynotvain.
