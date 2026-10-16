#!/bin/sh
# Optional first argument: unmodified VCPE tree for an exact effects-output comparison.
set -eu
H=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
V=$(CDPATH= cd -- "$H/../.." && pwd)
D=$(mktemp -d /tmp/vcpe-memory.XXXXXX)
trap 'rm -rf "$D"' EXIT HUP INT TERM
CC=${CC:-cc}
FLAGS="-O2 -Wall -Wextra -Wno-unused-parameter -ffunction-sections -fdata-sections ${CFLAGS:-}"
effects() {
    EXTRA=
    if [ "$2" = after ]; then EXTRA="$V/src/vcpe_adapt.c"; fi
    "$CC" $FLAGS -I"$H/stubs" -I"$1/src" \
        "-DENGINE_PART=\"$1/runtime/w_part.c\"" "-DENGINE_RENDER=\"$1/runtime/w_render.c\"" \
        ${3:-} "$H/effects.c" $EXTRA -Wl,--gc-sections -lm -o "$D/effects-$2"
    "$D/effects-$2" > "$D/$2.txt"
    cat "$D/$2.txt"
}
effects "$V" after '-DMEMORY_OPTIMIZED -DADAPT_CHECKS'
if [ "$#" -gt 0 ]; then
    effects "$(CDPATH= cd -- "$1" && pwd)" before
    test "$(sed -n '1p' "$D/before.txt")" = "$(sed -n '1p' "$D/after.txt")"
    echo 'Before/after particle squares and trail vertices are identical (600 frames)'
fi
"$CC" $FLAGS -Wno-deprecated-declarations -I"$H/stubs" -I"$V/src" \
    "$H/arena.c" "$V/src/vcpe_mem.c" -Wl,--gc-sections -o "$D/arena"
"$D/arena"
"$CC" $FLAGS -I"$V/src" "$H/save.c" -o "$D/save"
(cd "$D" && ./save)
"$CC" $FLAGS -I"$V/src" "$H/adapt.c" "$V/src/vcpe_adapt.c" "$V/src/vcpe_lod.c" -lm -o "$D/adapt"
"$D/adapt"
"$CC" $FLAGS -I"$H/stubs" -I"$V/src" "$H/sprite_trim.c" -Wl,--gc-sections -lm -o "$D/sprite-trim"
"$D/sprite-trim"
