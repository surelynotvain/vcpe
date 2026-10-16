# VCPE 5.0 release verification

The source tag is `vcpe-5.0`. The release's standalone demo archive contains
`PSP/GAME/VCPE50/EBOOT.PBP`, instructions, checksums, the license and AI policy. It needs no game data.
The engine sources and portable tests are in the repository and GitHub's source archives.

## Build and host checks

From a clean checkout, with a C compiler, Python 3, Pillow, NumPy, Fontconfig, a system TrueType font,
Podman and the `ghcr.io/pspdev/pspdev:latest` image:

```sh
sh tests/run.sh
CC=clang CFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' sh tests/culling/run.sh
sh blank/build.sh
```

The font-cache test obtains `stb_truetype.h` from the toolchain container unless `VCPE_STB_INCLUDE`
points to a local header directory. The culling suite runs only CPU code and mocked GU calls.
Its strict/fast-math paths cover 20,000 randomized camera/affine cases against an independent eight-corner
oracle, six planes and edge cases, invalid inputs, animated LOD invalidation, allocation/budget failure,
scene registration, changing labels, retained index order/coalescing and omitted hidden texture binds.

The host suite also checks memory/adaptation, deterministic contact/state hashes, cached transforms and
draw order, stable stores, font allocation/eviction, real-thread loading, prediction ownership, diagnostics,
input, converters, math, local saves, upgrade Makefile evaluation/idempotence/tamper detection/rollback,
and volatile-memory handoff. The PSP Blank Engine build links the native utility backend without warnings.
The release's 3D/culling/font/node objects are also cross-compiled with PSPSDK and fast-math flags.

## Standalone emulator checks

Set `PSP_EMU` to an installed `PPSSPPHeadless` binary. These scripts build and run engine-only fixtures,
without any demake or physical card:

```sh
PSP_EMU=/path/to/PPSSPPHeadless sh tests/v50/run.sh
PPSSPP_PSP1000=1 PSP_EMU=/path/to/PPSSPPHeadless VCPE50_OUT=/tmp/vcpe50-psp1000 sh tests/v50/run.sh
PSP_EMU=/path/to/PPSSPPHeadless sh tests/selftest/run.sh
```

`tests/v50/out/` receives logs, captures and generated save fixtures; these are not source files.
During 5.0 preparation, standalone PSP-1000 and later-model emulation passed native savedata round trips,
wrong-version/too-small-buffer rejection, preserved legacy import, metadata/icons, volatile handoff and
bundle reload. Loader captures differed and showed the supplied animation, credit and eight status rows.
The animation stores all 31 GIF frames with the original 3100 ms loop, scaled to 128x72 and edge-padded
to complete PSP textures (497 KiB including the palette). The broader self-test passed batch/pipeline
frame comparisons, LOD tolerance and moving loading animation.

Publication checks rerun the host suites and PSP builds. The new camera-culling addition has CPU/mocked-GU
and cross-compiler evidence; no complete-game replay or measured gameplay speedup is claimed. Emulator results
do not establish physical GE cache behavior or storage durability. Retest each upgraded game's own content.

## Physical-console checks still required

No physical PSP or card was used for this release. In the standalone demo, move right to SAVES:
Cross saves, Circle loads and Triangle previews the loader. Exit to XMB and inspect the demo in Saved Data
Utility: icon, title, slot description, copy and deletion. Check cancellation, sleep/resume and storage errors
on each supported console. Game-owned volatile allocations must observe `vcpe_resume_gen`.

The preparation headless emulator omits its utility text atlas, so dialog captures verify icon/geometry and
utility I/O, while final firmware text/XMB presentation remains a physical-console acceptance check.
See [upgrading-to-5.0.md](upgrading-to-5.0.md), [savedata.md](savedata.md), and
[camera-culling.md](camera-culling.md) for adoption, ownership and invalidation rules.
