# VCPE 5.1 release verification

Changes are listed in `CHANGELOG.md`.

## Host suites (Clang; sanitizers where the script enables them)

```sh
for t in skinning culling vehicle perspective capture memory v47; do CC=clang sh tests/$t/run.sh; done
python3 tests/icon1/run.py
python3 tests/native-data/run.py
```

All pass for this release: skinning (weights, UV/colour preservation, loop/clamp, half matrices, bounds),
culling (allocation failure fallbacks, no-culling compatibility), vehicle (arc length, heading, path
extension, rear-axle trailing and settling, stop), perspective/raycast contracts, capture staging,
memory/arena/save, 4.7 runtime/font/loading, PMF round trips with exact PTS/DTS, native data tables.

## PSP builds

`sh blank/build.sh` builds the Blank Engine (`made with VCPE 5.1`). The new modules (`vcpe_skin`,
`vcpe_path`, `vcpe_vehicle`, `vcpe_backdrop`, `vcpe_profile`, `vcpe_canvas`, `vcpe_offline`) compile
with `psp-gcc -Wall -Wextra` without warnings.

## Not verified

No physical PSP timing or stability test was made for the new 3D modules in this release. Emulator results
are not hardware results. Check performance on the target console after upgrading.
