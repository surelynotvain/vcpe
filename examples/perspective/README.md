# VCPE Perspective Lite 1.02

Conservative replacement for the original perspective demonstration after a reported
physical PSP-3000 freeze after the scene appeared. The cause is not yet confirmed.

The default scene uses 120 ordinary 2D wall strips, sprite cutouts and flat floors
and ceilings. It does not submit GPU triangles or use frame pipelining. A fixed
4 MiB heap replaces the previous near-total-memory request and startup heap probe.
The display list is 128 KiB; scene scratch is 4,448 bytes; the swizzled shared T8
atlas is 33,792 bytes. The earlier demo used 84 KiB of projected-triangle storage,
a 512 KiB display list, and 48 KiB of textures. Framebuffers, code, loader art,
stack and other engine state are additional costs in both builds.

Build: `sh build.sh` (PSPdev Podman image).
Test: set `PSP_EMU` to PPSSPPHeadless, then `sh verify.sh`.
Install only EBOOT.PBP in `PSP/GAME/FLATDEMO/`. No data PAKs or AUTOTEST.TXT needed.
Retain the backed-up EBOOT if you want to compare; this build has title
and DISC_ID UCJS19053, version 1.02, MEMSIZE=0, 4 MiB heap, 128 KiB main stack.

Analog stick moves; D-pad up/down moves and left/right turns; L/R strafe;
Triangle/Square look; Cross opens the second door when near it; Start pauses;
Select shows performance. HOME exits.

The native program writes FLAT.LOG beside its EBOOT. Startup and the first two
frames are logged at submission/completion boundaries, then every 600 frames.
If it still hangs, the final log line helps distinguish startup from GPU submission.
The absence of a log may indicate failure before the application starts.

Host sanitizer tests, normal-memory and 32 MB PPSSPP traversal/camera sweeps pass.
This is a generic engine test scene, not a game demake, and the updated
build has not yet been confirmed on physical hardware.

## 1.02 hardware diagnosis

The mounted PSP card contained the emulator's AUTOTEST.TXT. That disables manual
input; its first capture created SNAPcourtyard.RAW with zero bytes. FLAT.LOG
recorded two completed frames on physical hardware (~8.4 ms build time), so the
available evidence points to the automated capture rather than a first-frame
render failure. The script is now retained as AUTOTEST.DISABLED on the card.

Normal startup ignores test scripts unless TESTMODE.ON exists or both triggers
are held at launch. The build shows MANUAL INPUT and a changing frame counter.
The normal VCPE loader is explicitly visible for about 0.65 seconds in this demo.
Capture/recording writes stage eight rows in ordinary aligned RAM; storage I/O
never receives VRAM pointers. Partial writes and errors are handled.

The source's 150 ms loader suppression is normal, so tiny linked-asset loads in
older builds completed before the animation could appear. No global loader timing
or automatic test policy of existing games was changed.
