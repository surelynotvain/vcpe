#!/bin/sh
# Public engine-only host checks. No private projects, game assets or hardware are needed.
set -eu
H=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
V=$(CDPATH= cd -- "$H/.." && pwd)
D=$(mktemp -d /tmp/vcpe-host.XXXXXX)
trap 'rm -rf "$D"' EXIT HUP INT TERM
CC=${CC:-cc}
FLAGS="-O2 -Wall -Wextra -Wno-unused-parameter ${CFLAGS:-}"
"$CC" $FLAGS -I"$V/src" "$H/test_input.c" -o "$D/input"
"$D/input"
python3 -m unittest discover -s "$H" -p 'test_*.py'
"$CC" $FLAGS -I"$V/src" "$H/host/math_test.c" "$V/src/vcpe_math.c" -lm -o "$D/math"
"$D/math"
"$CC" $FLAGS -I"$V/src" "$H/host/save_test.c" "$V/src/vcpe_save.c" -o "$D/save"
"$D/save" "$D"
"$CC" $FLAGS -I"$H/host/stub" -I"$V/src" "-DSRC=\"$V/src/vcpe_audio.c\"" \
    "$H/host/audio_equiv.c" -lm -o "$D/audio"
"$D/audio" "$D/audio.pcm"
if [ -n "${VCPE_AUDIO_BASELINE:-}" ]; then
    "$CC" $FLAGS -I"$H/host/stub" -I"$V/src" "-DSRC=\"$VCPE_AUDIO_BASELINE\"" \
        "$H/host/audio_equiv.c" -lm -o "$D/audio-before"
    "$D/audio-before" "$D/audio-before.pcm"
    cmp "$D/audio-before.pcm" "$D/audio.pcm"
    echo 'PASS 400-block mixer output matches baseline exactly'
fi
"$CC" $FLAGS -I"$V/src" "$H/host/ultra_test.c" "$V/src/vcpe_ultra.c" -o "$D/ultra"
(cd "$D" && ./ultra)
sh "$H/host/jobs_pack_test.sh"
sh "$H/host/lod_test.sh"
python3 "$H/native-data/run.py"
sh "$H/memory/run.sh"
sh "$H/v47/run.sh"
sh "$H/culling/run.sh"
python3 "$H/v50/upgrade.py"
"$CC" $FLAGS -ffunction-sections -fdata-sections -Wno-deprecated-declarations \
    -I"$H/memory/stubs" -I"$V/src" "$H/v50/volatile.c" "$V/src/vcpe_mem.c" \
    -Wl,--gc-sections -o "$D/volatile"
"$D/volatile"
sh "$H/v52/run.sh"
echo 'PASS VCPE 5.0 host suites'
