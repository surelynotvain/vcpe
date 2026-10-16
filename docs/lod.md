# LOD meshes (`vcpe_lod`)

One mesh, several detail levels. Far away, neighbouring triangles are joined while the change stays under about a
pixel on screen, so the GE draws fewer triangles and nobody sees the difference. Up close the full mesh comes back.

## Offline: `tools/vcpe_lod.py`
```sh
python3 tools/vcpe_lod.py crate.obj crate.vld            # 5 levels, each about half the previous one
python3 tools/vcpe_lod.py crate.obj crate.vld --levels 6 --ratio 0.6 --min-tris 16
```
or from a converter: `blob, info = build_lod(verts, idx)` with VcpeVtx tuples `(u, v, abgr, x, y, z)`.

- Triangles are joined by half-edge collapses ranked by quadric error. No new vertices are made, and the vertices
  are reordered so every level only uses a prefix of one shared vertex array. Each level adds only its indices.
- Collapses that would flip a triangle, pinch the surface, or move an open edge or a UV/colour seam are refused.
  Outlines and texture seams therefore never move, but a mesh with many seams cannot be reduced as far.
- A level's error is measured: the largest distance between it and the full mesh, both ways, in model units.
- It's pure Python with numpy, so it's slow: a 2 000-triangle mesh takes about 10 s. Run it once at conversion time.

Put the `.vld` blob in the PAK or in a pack (`vcpe_packer`, kind data). It must stay in memory, 16-byte aligned.

## Runtime
```c
VcpeLodMesh rock;
vcpe_lod_open(&rock, vcpe_bundle_mem(B_ROCK), size);   /* 0 = ok */

vcpe_3d_begin(cam, rot, 60, 0.1f, 200);
VcpeLodView view;
vcpe_3d_lod_view(&view, 1.0f);                         /* this camera, 1 pixel of error allowed */
for (each rock r) {
    float c[3];   /* rock.centre through r's model matrix */
    r->lod = vcpe_lod_pick(&rock, &view, c, r->scale, r->lod);   /* start r->lod at -1 */
    vcpe_3d_model(r->matrix);
    vcpe_3d_draw_lod(page, &rock, r->lod);
}
```
- An error `e` at distance `d` covers `e x px_k / d` pixels (`px_k` = half the screen height over tan of half the
  FOV). The distance is to the nearest point of the bounding sphere. The coarsest level under the threshold wins.
- Hysteresis: going coarser needs the error under `thresh x down_k` (0.75), going finer happens as soon as it passes
  the threshold. Pass last frame's level back so a mesh does not flicker at a boundary distance.
- `thresh_px` is the quality knob. 1 px cannot be told apart in practice. 2–3 px is fine for fast scenes. It is
  a plain float, so it can be bound to live tuning (`vcpe_tune_float("lod.px", &view.thresh_px, 0.5f, 8)`).
- Stats: `vcpe_lod_stat_full` / `vcpe_lod_stat_drawn` (triangles per frame, reset by `vcpe_3d_begin`).

## Linking and PSPSDK
`vcpe_lod.c` is plain C with no PSP header. It is tested on the host (`tests/host/lod_test.sh`). The only GPU call is
`vcpe_3d_draw_lod` in `vcpe_3d.c`, which goes through `vcpe_3d_draw`. If the 3D backend later stops using sceGu, the
LOD code doesn't change. Games link `vcpe_lod` only when they call `vcpe_lod_open` / `vcpe_lod_pick`. 3D games that don't
use LOD need nothing new.

## Tests
- `tests/host/lod_test.sh`: the tool on a UV sphere and an OBJ grid, then 17 checks (layout, vertex prefixes,
  distance picking, error always within 1 px, hysteresis, damaged blobs).
- Self-test scene 3: the sphere at 5 distances on the PSP (levels 0..4, 4278 of 11040 triangles drawn). The frame is
  compared with the same frame at full detail. Only single silhouette pixels flip, and interior shading moves by
  less than 8 of 255.
