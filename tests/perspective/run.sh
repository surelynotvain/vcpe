#!/bin/sh
set -eu
H=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
V=$(CDPATH= cd -- "$H/../.." && pwd)
D=$(mktemp -d /tmp/vcpe-perspective.XXXXXX)
trap 'rm -rf "$D"' EXIT HUP INT TERM
for mode in strict fast; do
 extra=; [ "$mode" != fast ] || extra=-ffast-math
 ${CC:-cc} -std=c99 -O2 -Wall -Wextra -Werror $extra ${CFLAGS:-} -I"$V/src" \
  "$H/contracts.c" "$V/src/vcpe_perspective.c" "$V/src/vcpe_sector.c" "$V/src/vcpe_math.c" -lm -o "$D/test"
 "$D/test"
done
${CC:-cc} -std=c99 -O2 -Wall -Wextra -Werror ${CFLAGS:-} -I"$H/stubs" -I"$V/src" \
 "$H/backend.c" "$V/src/vcpe_perspective.c" "$V/src/vcpe_perspective_draw.c" "$V/src/vcpe_math.c" -lm -o "$D/backend"
"$D/backend"
for mode in strict fast; do
 extra=; [ "$mode" != fast ] || extra=-ffast-math
 ${CC:-cc} -std=c99 -O2 -Wall -Wextra -Werror $extra ${CFLAGS:-} -I"$V/src" \
  "$H/raycast.c" "$V/src/vcpe_raycast.c" "$V/src/vcpe_math.c" -lm -o "$D/raycast"
 "$D/raycast"
done
