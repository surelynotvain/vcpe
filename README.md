# Vain's C Portable Engine 5.1

Native C engine modules for PSP homebrew and demakes. Include `src/vcpe.h`, then link the
modules used by your game through `VCPE_MODULES`. Targets PSPSDK, firmware 6.60; the
standalone Blank Engine needs no game files.

Build the engine demonstration:

```sh
sh blank/build.sh
```

This uses the locally installed `ghcr.io/pspdev/pspdev:latest` Podman image and writes
`blank/EBOOT.PBP`. Copy that EBOOT to `PSP/GAME/VCPE51/` to run it. Left/right changes
pages; the SAVES page opens native save/load dialogs and previews the loading screen.

5.1 adds real 3D skinning, materials and
filtering, display-list guards, and new `vcpe_path`, `vcpe_vehicle`, `vcpe_backdrop`, `vcpe_profile`, `vcpe_canvas`
and `vcpe_offline` modules. See [upgrading to 5.1](docs/upgrading-to-5.1.md).

5.0 completes the memory/adaptation and 4.7 runtime improvements and adds the standard
Hetalia Czechia loading animation, “made with VCPE 5.0”, an eight-row status log,
reversible per-project engine upgrades, and native PSP savedata.

The local 5.0 update also includes [automatic camera culling](docs/camera-culling.md): geometry-derived batch/chunk
bounds, affine camera tests and mutable-pose invalidation. It has host/build checks; gameplay verification is pending.

- [Flat perspective cards and directional sprites (local extension)](docs/flat-perspective.md)
- [Upgrade an older demake](docs/upgrading-to-5.0.md)
- [Native savedata and XMB presentation](docs/savedata.md)
- [Release verification and commands](docs/release-5.1.md) ([5.0](docs/release-5.0.md))
- [Changes](CHANGELOG.md)
- [Automatic budgets](docs/adaptive-budgets.md), [memory](docs/memory-optimizations.md)
- [Blank Engine](docs/blank-engine.md), [PSP-1000](docs/psp1000.md)

Engine upgrades do not rewrite gameplay or convert save schemas automatically.
Keep each released game pinned to its tested engine snapshot. Emulator checks and
real PSP hardware checks are separate; see the release verification document.
