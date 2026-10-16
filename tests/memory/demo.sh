#!/bin/sh
# Build and drive only the standalone VCPE Blank Engine. PSP_EMU must point to PPSSPPHeadless (or be on PATH).
set -eu
H=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
V=$(CDPATH= cd -- "$H/../.." && pwd)
EMU=${PSP_EMU:-$(command -v PPSSPPHeadless || true)}
[ -n "$EMU" ] || { echo 'Set PSP_EMU to the PPSSPPHeadless executable.' >&2; exit 1; }
D=${1:-$(mktemp -d /tmp/vcpe-adapt-demo.XXXXXX)}
mkdir -p "$D"
D=$(CDPATH= cd -- "$D" && pwd)
G=$D/PSP/GAME/VCPEADAPT
mkdir -p "$G"
(cd "$V/blank" && sh build.sh) > "$D/build.log" 2>&1
cp "$V/blank/EBOOT.PBP" "$G/EBOOT.PBP"
cat > "$G/AUTOTEST.TXT" <<'S'
adapt 1
wait 5
adaptcheck 256
snap full
adaptfeed 35000 8192 500
adaptcheck 128
wait 2
snap low
adaptload
wait 2
snap reclaimed
adaptfeed 16683 8192 1600
adaptcheck 256
wait 2
snap recovered
quit
S
timeout 60 "$EMU" --memstick="$D" --timeout-emulated=15 --graphics=software "$G/EBOOT.PBP" > "$D/run.log" 2>&1
cat "$D/run.log"
! rg -q '^FAIL' "$D/run.log"
test "$(rg -c '^PASS adapt scale' "$D/run.log")" = 3
rg -q '^PASS adaptive loading progress work' "$D/run.log"
rg -q 'reclaimed 64 KB' "$D/run.log"
for name in full low reclaimed recovered; do test -s "$G/SNAP$name.RAW"; done
echo "Engine-only demo passed; snapshots and logs: $D"
