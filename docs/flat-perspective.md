# Flat perspective scenes (local optional extension)

This path renders textured cards and upright sprite cutouts through the PSP's **2D
triangle** drawing mode. The CPU projects and clips the cards. There is no hardware
depth buffer, GU matrix stack, skinned mesh, normal map or dynamic light pass.
The existing `vcpe_3d` path stays available and is unchanged. The release version
remains 5.0; this is a local, unreleased, additive engine extension.

## Current example: conservative wall-strip fallback

After a reported physical PSP-3000 freeze after the first scene appeared, the
example now defaults to **Perspective Lite 1.02**. Its cause is not confirmed.
The generic card renderer remains available; the default example uses
`vcpe_raycast` for ordinary 2D sprite strips, without GPU triangles or pipelining.
The replacement has a fixed 4 MiB heap and omits the large-allocation heap probe.
It has 4,448 bytes of scene scratch, 33,792 bytes of shared T8 textures, and a
128 KiB display list. Its floor/ceiling are flat fills. Per-strip wall distances
occlude sprites; these are not a framebuffer depth buffer. This is deliberately
simpler than the first example. This change does not alter other games' heap,
renderer, memory-model checks or pipeline settings.

```sh
sh tests/perspective/run.sh
CC=clang CFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' sh tests/perspective/run.sh
sh examples/perspective/build.sh
PSP_EMU=/path/to/PPSSPPHeadless sh examples/perspective/verify.sh
PPSSPP_PSP1000=1 PSP_EMU=/path/to/PPSSPPHeadless VCPE_FLAT_OUT=/tmp/vcpe-lite-lowmem sh examples/perspective/verify.sh
```

Copy `examples/perspective/EBOOT.PBP` to `PSP/GAME/FLATDEMO/`. No PAKs are needed.
SFO: firmware 6.60, title VCPE Perspective Lite, version 1.02, MEMSIZE=0,
DISC_ID UCJS19053. The demo does not require the extra RAM region. This is an
engine diagnostic with procedural art, separate from any actual game conversion.

Controls: stick moves; D-pad up/down moves and left/right turns; L/R strafe;
Triangle/Square looks up/down; Cross operates the second door nearby; Start
pauses; hold Select for timing. HOME exits. Startup and frame-boundary diagnostics
are written to `FLAT.LOG` next to the EBOOT, with only periodic writes during play.

The regression walks three cells, verifies the closed door, opens and crosses it,
takes six screenshots, and sweeps the camera for 1,200 frames. Both normal and
32 MB emulator configurations pass. A physical PSP retest remains necessary.

## Modules and ownership

Add these to the project's `VCPE_MODULES`:

```
vcpe_perspective vcpe_perspective_draw vcpe_sector vcpe_math
```

The normal VCPE graphics, loading, input, system, test and HUD modules are still
required by the application. The upgrade tool resolves the new modules' engine
dependencies. The perspective path does not depend on `vcpe_3d`, `vcpe_node3d`,
`vcpe_cull`, `vcpe_lod` or `libpspgum`.

1. Reserve a `VcpePerspectiveTriangle` queue once behind the standard loading screen.
2. Call `vcpe_perspective_begin` every frame with the camera and queue.
3. Submit flat textured quads through `vcpe_perspective_card`, or cutouts through
   `vcpe_perspective_sprite`. UV coordinates are normalized to the selected page.
4. Inside `vcpe_gfx_begin/end`, call `vcpe_perspective_draw` before drawing the HUD.

World axes are Y-up. Yaw 0 faces -Z, positive yaw turns toward -X, and positive pitch
looks up, matching the existing 3D camera convention. Corners follow perimeter order;
there is no backface culling. Six-plane clipping happens before projection, including
near/far planes, so crossing a card does not cause division by zero or a giant screen
triangle. The queue rejects a whole card atomically if it cannot fit. Inspect `dropped`:
capacity, absent textures and display-list exhaustion are observable failures.

The draw adapter copies projected vertices into frame-owned display-list memory;
it never leaves GPU pointers into the CPU queue. Thus the next update may reuse the
queue even with pipelining. It batches adjacent same-page triangles in sorted order,
up to 64 triangles per submission, and increments standard draw-call statistics.
Textures keep the existing VCPE bundle ownership/cache rules. Scene loading and
replacement must use the normal loading screen and GPU wait rules.

`vcpe_perspective_view` selects 1..32 angles from actor heading and camera position.
For eight views, frame 0 sees the front, 2 the left-facing camera direction (-X),
4 the back, and 6 the opposite side. Artwork may use a caller-provided UV subrectangle.

## Resource budget and content rules

The original triangle example reserved 86,016 bytes (84 KiB) for 1,024 projected
triangles and 49,152 bytes (48 KiB) for three RGBA4444 texture pages. Those are fixed bounds,
not an estimate for a completed game. The rest of the engine, framebuffers, display
list, code, audio and game state consume additional memory. The renderer does not
reserve the 278,528-byte (272 KiB) depth region used by `vcpe_3d`.

Most objects should be a single cutout or a few planes. Use directional baked views
for characters and complex props, and tiny authored floor/wall meshes for rooms.
Split long slanted surfaces into a few cards to reduce affine texture warping.
Prefer 32/64/128 pixel indexed or RGBA4444 images to large RGBA8888 assets.
Load only the current area and necessary neighbors through ordinary VCPE bundles.

**Painter sorting is not a general hidden-surface solver.** Triangles sort by average
camera depth. Intersecting cards, cyclic overlaps, a large floor cutting through a wall,
or arbitrary overlapping room geometry can draw in the wrong order. Split those
surfaces when authoring content, keep room depth small, and constrain cinematic camera
paths. Perspective-correct texturing is approximated by subdivisions. Directional
sprites have discrete view changes. Transparent cutouts discard alpha <= 0x20.
No general NIF/model converter or full 3D occlusion guarantee is provided here.

## Cells and collision

`vcpe_sector` is allocation-free floor-plan support, capped at 64 nonoverlapping
rectangular X/Z cells and 256 axis-aligned portals. Validate authored data at loading.
A portal must lie on the shared boundary of two cells, with coordinates in ascending
order. The `open` flag controls both connectivity and collision. Closed-door art is
submitted separately by the game, using the same flag.

Bodies have radius, height and allowed step up/down. Movement uses bounded substeps,
checks doorway width and headroom, and slides along walls by separating X/Z movement.
Requests too large for 128 substeps are rejected without changing the body; split
long moves across fixed updates. Vertical motion is a floor-height snap. Jumping,
slopes, stairs with continuous motion, stacked rooms, moving platforms, hit detection
and actor-vs-actor collision belong in additional systems.

`vcpe_sector_connected` provides a conservative connectivity mask, **not portal
occlusion or a streaming manager**: every cell reachable through open doors is retained.
Use it before submitting disconnected cells; the projector then clips off-screen cards.
A content system must still decide which areas are resident.

## Verification boundaries

Host tests cover clipping at camera planes, invalid floats in fast-math builds,
queue overflow, painter ordering, eight headings, door/cell movement, step/headroom
limits and tunnelling rejection. A mocked GU test enforces 2D-only draw flags,
normalized-to-pixel UV conversion, frame checks and allocation/texture failures.
Clang AddressSanitizer and UndefinedBehaviorSanitizer exercise both CPU and adapter tests.
Native PSP builds and PPSSPP software-renderer captures verify the integrated path.
The demo's main translation unit uses the SDK's
[`PSP_DISABLE_NEWLIB_SOCKET_SUPPORT()`](https://github.com/pspdev/pspsdk/blob/master/src/user/pspmoduleinfo.h)
macro. Its build checks the ELF symbol list for accidental Inet dependencies, full
3D modules, GU matrix calls and depth-buffer setup. This is opt-in for the example;
other engine users retain their existing SDK networking settings.
Engine 5.0 host/build/savedata/loader regression checks also pass after the additions.

Physical PSP-3000 speed, memory-stick behavior and firmware sleep/resume have not
been verified for this example. Test on the console and read the Select overlay;
emulator timings do not establish hardware frame rate.

## Generic wall-strip API

Link `vcpe_raycast vcpe_math` alongside normal graphics modules. Supply line
segments with floor/top heights, a camera and caller-owned columns. A four-pixel
strip width uses 120 columns at 480x272. `vcpe_raycast` returns perpendicular
distance, wall index, horizontal texture coordinate and projected top/bottom.
The application submits these as clipped `vcpe_quad` sprites and can use column
distance to hide sprite strips behind walls. Flat floors/ceilings are intentional;
this backend does not reproduce overhangs, layered lintels or stacked rooms.
`near_z`, `far_z`, capacity, world bounds and finite inputs are checked. There is
no heap allocation. The math validation uses integer float-magnitude bits so it
still rejects nonfinite values under fast-math.

The card renderer additionally has an opt-in `vcpe_perspective_draw_ex` opaque
mode for opaque/cutout-only scenes; default drawing still blends. Corner outcodes
fast-reject hidden cards and bypass clipping scratch for wholly visible cards.
A 32,000-card A/B comparison across 4,000 cameras matches the prior clipped
vertices and counts. Unchanged queues do not need to be sorted again; callers
that edit queue entries directly must clear `sorted` before their next draw.

## Physical-card findings and 1.02

The installed directory contained AUTOTEST.TXT and emulator screenshots. Its
FLAT.LOG showed successfully completed frames 0/1, while SNAPcourtyard.RAW was
zero bytes. The first scripted screenshot is thus the suspected stall point;
manual controller input was also disabled by the script. Preserve these findings
separately from the still-pending physical retest of 1.02.

This example now requires TESTMODE.ON or both boot triggers before loading
AUTOTEST.TXT. The verify script creates the opt-in marker only in its evidence
memstick. Playtest packages reject script/marker/RAW/LOG files. The loader is
forced visible for 650 ms at boot and the HUD exposes a live frame count.
`vcpe_test` stages framebuffer captures in 15 KiB of transient aligned RAM before
sceIoWrite, frees it after capture and handles partial/failing writes. The static
scene budget is unchanged; that RAM is only allocated for an explicit capture.
A sanitizer test enforces that I/O never receives a source framebuffer pointer.
Native emulator captures and the 5.0 savedata/loader regression still pass.
