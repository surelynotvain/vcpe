# Reservations and compact loading (4.1)

## Reservations (`vcpe_jobs`)
Background work in three words: **clients** book **reservations**, **servers** hand them out.

```c
int ai = vcpe_client_add("ai", 70);          /* name + priority 0..100; client 0 is "default" (50) */
int loader = vcpe_client_add("loader", 30);
vcpe_servers_start(2, 0);                     /* 2 worker threads, below the main thread's priority */

vcpe_reserve(loader, unpack_level, &lvl, level_ready, 0);   /* fn runs on a server, done on the main thread */
vcpe_reserve(ai, plan_path, &enemy[0], path_ready, 0);
vcpe_reserve(ai, plan_path, &enemy[1], path_ready, 10);     /* urgency +10 */
VCPE_RESERVE(fn, arg);                                       /* the one-liner: default client, no callback */

/* every frame */
vcpe_jobs_update();                           /* delivers finished work (done callbacks), ages what waits */
```

- **Automatic priority**: client priority + urgency + 1 point per 4 frames waited (`vcpe_jobs_age_frames`). The
  best pending reservation goes to the next free server; equal priorities go oldest first. Nothing starves.
- **Rules for the job function**: it runs on another thread. Touch only its own data, and do no drawing or audio
  calls. The done callback runs on the main thread and may use everything.
- **On a single-core PSP**, servers use the time the main thread waits: the vblank, the GE, file reads. A job
  that reads the memory stick, decompresses or searches paths finishes "for free" during those waits.
- No servers (`vcpe_servers_start(0)`, or never called): `vcpe_jobs_update` runs reservations itself, best first,
  for up to `vcpe_jobs_budget(us)` per frame (default 4 ms). Same code, no threads.
- `vcpe_reserve_cancel`, `vcpe_reserve_state`, `vcpe_jobs_wait`, `vcpe_jobs_wait_all`, `vcpe_jobs_line` (stats per
  client). With `vcpe_ultra` linked, completions also post `VCPE_EV_JOB_DONE`.
- Limits: 16 clients, 64 reservations at once, 4 servers (64 KB stack each).

## Compact loading (`vcpe_pack`)
```python
from vcpe_packer import build_pack, KIND_TEXTURE      # tools/vcpe_packer.py
forest = build_pack([('tree.tex', tex), ('wind.adp', snd)])   # into the PAK with vcpe_pak.write_pak
```
```c
vcpe_pack_pool(0, 6 << 20, 3);                            /* once: 3 slots of 2 MB, never freed while playing */
int h = vcpe_pack_load(PAK_FOREST_OFF, PAK_FOREST_SIZE, 60);   /* one read */
const void *tex = vcpe_pack_find(h, "tree.tex", &size);  /* binary search by name hash, 64-byte aligned data */
vcpe_pack_unload(h);                                      /* instant */
vcpe_pack_load_async(off, size, 40, ready, user);        /* read on a server thread, ready() on the main thread */
```
- **Priority list**: when every slot is full, the pack with the lowest priority, then the least recently used
  (`vcpe_pack_find` and `vcpe_pack_touch` count as uses), is evicted. Pinned packs (`vcpe_pack_pin`) and packs
  still loading are never evicted. `vcpe_pack_line` prints the list.
- Handles carry a generation number: a handle to an evicted pack reads as `VCPE_PACK_FREE`, never as another pack.
- Loaded packs are written back from the CPU cache, so textures in them can go straight to the GE.
- Each pack is checked (magic, version, size, table bounds) before use.
