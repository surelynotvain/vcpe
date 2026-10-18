# Automatic camera culling

Link `vcpe_cull` alongside `vcpe_3d` and `vcpe_node3d`. The easy-upgrade module now resolves that dependency
for 3D projects. The optional module is enabled by default when linked; old clients that omit it still link and
keep their original drawing path. The current local engine version remains 5.0.

Bounds are derived from the actual indexed positions. Entirely outside batches are rejected before texture binding
or GU submission. Intersecting batches are checked in contiguous 128-triangle chunks; adjacent retained ranges
coalesce into one draw. Indices, triangle order, material order, alpha settings and LOD topology remain unchanged.
Objects intersecting an edge or near plane are retained. Camera movement/FOV/rotation are evaluated each frame;
planes transformed into object space handle translations, mirrored/nonuniform scale and parent-induced shear.
World-space sphere queries also use the conservative plane tests when the module is linked.

Scene-node batches prepare themselves at scene load and when a mesh/LOD is first used. Raw 3D batches can warm
their cache behind a loading screen:

```c
vcpe_loading_begin(0);
vcpe_3d_prepare(vertices, vertex_count, indices, index_count);
/* Prepare every alternate LOD index list that shares these vertices too. */
vcpe_loading_end();
```

`vertices` uses the engine's 24-byte `VcpeVtx` layout, and indices are 16-bit triangle lists. No hand-authored
visibility boxes are needed. Geometry must remain alive while registered. Call `vcpe_3d_geometry_changed(vertices)`
after changing positions or indices; this marks all cached LODs sharing that vertex base for allocation-free refit
before their next draw. Model-only movement needs no invalidation. Do not cache a mutable buffer without issuing
that invalidation. 3D font labels use current transient glyph bounds instead of a persistent cache.

`vcpe_node3d_load` resets the shared geometry cache to prevent stale scene data. Register raw batches after that
call. Before freeing or replacing assets yourself, call `vcpe_3d_cull_reset()` (or `vcpe_cull_forget` for individual
vertex bases). Reused allocation addresses must not inherit the previous asset's bounds.

Memory: the PSP bucket table is 1 KiB BSS. Entries and chunk bounds grow on the heap under a default 512 KiB cap.
`vcpe_cull_limits(bytes, chunk_triangles)` changes the cap/chunk size and resets the cache. Chunk sizes round up
to multiples of eight so index starts retain the original buffer's 16-byte alignment. Cache misses, invalid bounds,
bad camera/model data or allocation failures retain the original draw. This module does not unload offscreen assets.

The detector tests the six camera planes; geometry hidden by another object inside the view is still submitted.
Animations, transforms, physics, game state and audio continue independently of render rejection.

`vcpe_3d_autocull(0)` bypasses automatic rejection for comparisons; `1` enables it. Per-frame counters:

- `vcpe_stat_tris`: indexed triangles actually submitted.
- `vcpe_stat_cull_tests`: registered draw batches tested.
- `vcpe_stat_culled`: batches or 3D labels fully rejected.
- `vcpe_stat_cull_tris`: known rejected batch/chunk/label triangles (does not include the earlier node sphere pass).
- `vcpe_stat_cull_nodes`: nodes rejected before batch submission.
- `vcpe_3d_cull_bytes()`: current cache allocation; detailed cache stats include fallback and refit counts.

Host-only verification, without game or emulator execution:

```sh
sh tests/culling/run.sh
CC=clang CFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' sh tests/culling/run.sh
```

The tests check 20,000 randomized camera/affine cases against an independent eight-corner oracle, edge/near/far
cases, nonfinite inputs, dynamic LOD invalidation, allocation/budget failures, scene-node registration, changing
labels and mocked GU submissions. They verify retained range order/coalescing and no binding for wholly rejected
draws. The module explicitly preserves finite-value checks under the engine's fast-math flags; GCC and Clang
paths are covered. No in-game replay, emulator launch or physical-console playtest was performed for this change.
Visual correctness and actual frame-time gains remain the playtester's acceptance checks.
