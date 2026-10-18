# Upgrading to 5.1

5.1 is source compatible with 5.0. No existing function changed signature; everything new is opt-in.

1. Preview: `python3 tools/vcpe_upgrade.py plan /path/to/game` (then `apply`; `rollback` restores the snapshot).
2. Add only the modules you use to `VCPE_MODULES`:

| Module | Use | Needs |
| --- | --- | --- |
| `vcpe_skin` | skinned meshes, clip playback, head aim | `vcpe_math` |
| `vcpe_path` | rails, carts, camera tracks along point chains | `vcpe_math` |
| `vcpe_vehicle` | towed wagons/carriages with trailing, grade pitch, bumps | `vcpe_path` |
| `vcpe_backdrop` | sky dome + distant ridge rings behind fogged 3D | `vcpe_3d` |
| `vcpe_profile` | p95 frame windows in test logs | — |
| `vcpe_canvas` | zoomed 2D drawing without touching engine files | `vcpe_spr` |
| `vcpe_offline` | file-only games: no sceNetInet import | — |

3. 3D games that draw many materials should set `-DVCPE_DLIST_KB=512` (or measure `vcpe_stat_dlist_full`).
   Raw GU code should call `vcpe_gfx_room(bytes)` before emitting commands.
4. `vcpe_backdrop`: call `vcpe_backdrop_build` once, then `vcpe_backdrop_draw(rot, fov)` right before
   the world's `vcpe_3d_begin`; set its `haze` to your fog colour.
5. `vcpe_vehicle`: update with simulation ticks only (`ticks = 0` frames skip the call) so pause holds the pose.
   The model origin is the front axle; seat offsets are in that space.

Keep released games pinned to their tested snapshot; re-test on hardware after upgrading.
