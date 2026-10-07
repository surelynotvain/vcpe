# Godot import helpers

Python helpers for converters that read Godot 4 projects (for example ones recovered with GDRE Tools), plus a
visibility tool. Nothing here is specific to one game.

| File | What it does |
|---|---|
| `tscn.py` | Reads a Godot 4 text scene: ext_resources, sub_resources and nodes with their raw properties; `surfaces()` lists an ArrayMesh's surfaces (format, counts, material) |
| `scene3d.py` | World transforms of all nodes (Transform3D rows), YXZ euler angles in degrees to a basis, numbers from Godot value strings |
| `animated.py` | The nodes any AnimationPlayer track moves or changes (resolves `root_node` and relative track paths), to tell static scenery from moving parts |
| `vcpe_shadow.c` | Baked shadows: for (point, light) pairs, whether a triangle blocks the light (BVH, any-hit rays; only front faces block, like shadow maps drawn with back faces culled). Build: `gcc -O2 -o vcpe_shadow vcpe_shadow.c -lm` |
| `vcpe_vis.c` | Software z-buffer: given triangles and camera view-projection matrices, marks every triangle that is in front at some pixel of some pose. Build: `gcc -O2 -o vcpe_vis vcpe_vis.c` |

Typical use: decode the static meshes in world space, render them from every camera pose the game can use
(plus poses in between and a slightly wider field of view), and drop the triangles that are never seen.
Moving parts are left out of the test so they never hide anything.
