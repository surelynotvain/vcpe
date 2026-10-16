#!/bin/sh
# CPU contracts and mocked GU submissions only. Does not launch a game or emulator.
set -eu
H=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd);V=$(CDPATH= cd -- "$H/../.." && pwd)
D=$(mktemp -d /tmp/vcpe-culling.XXXXXX);trap 'rm -rf "$D"' EXIT HUP INT TERM
CC=${CC:-cc}
FLAGS="-O2 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections ${CFLAGS:-}"
for math in strict fast;do
    EXTRA=;if [ "$math" = fast ];then EXTRA=-ffast-math;fi
    "$CC" $FLAGS $EXTRA -I"$V/src" "$H/geometry.c" "$V/src/vcpe_cull.c" -lm -o "$D/geometry-$math"
    "$D/geometry-$math"
done
"$CC" $FLAGS -ffast-math -I"$H/stubs" -I"$V/src" "$H/render.c" "$V/src/vcpe_cull.c" -Wl,--gc-sections -lm -o "$D/render"
"$D/render"
"$CC" $FLAGS -I"$V/src" "$H/allocation.c" -lm -o "$D/allocation"
"$D/allocation"
"$CC" $FLAGS -Wno-address -Wno-unused-function -DNO_CULL_MODULE -I"$H/stubs" -I"$V/src" "$H/render.c" -Wl,--gc-sections -lm -o "$D/compat"
"$D/compat"
