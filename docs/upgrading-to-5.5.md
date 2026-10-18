# Upgrading to 5.5

Source compatible with 5.1. Add only the modules you use to `VCPE_MODULES`
(`python3 tools/vcpe_upgrade.py plan /path/to/game` resolves dependencies):

| Module | Use | Needs |
| --- | --- | --- |
| `vcpe_rig` | skeletons, compressed clips, IK, skinned meshes | `vcpe_skin` |
| `vcpe_animator` | per-character layered playback, foot IK | `vcpe_rig` |
| `vcpe_terrain` (+`vcpe_terrain_draw`) | heightfield cells | `vcpe_3d` for drawing |
| `vcpe_cells` | open-world streaming | optional `vcpe_pack`, `vcpe_jobs` |
| `vcpe_collide` | static collision, rays, character controller | - |
| `vcpe_sky` (+`vcpe_sky_draw`) | day cycle, weather, rain/snow | `vcpe_3d` for drawing |
| `vcpe_crowd` | NPC level of detail | optional `vcpe_adapt` |

PSP-1000 (about 20 MB): keep `vcpe_cells` radius 1-2 with 16-32 quad cells, `vcpe_crowd` max_full 4.
Raise `-DVCPE_DLIST_KB` to 512 with terrain plus precipitation. Re-test released games on hardware.
