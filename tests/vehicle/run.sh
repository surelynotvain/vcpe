#!/bin/sh
set -eu
H=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
O=${TMPDIR:-/tmp}/vcpe-vehicle-test
${CC:-cc} -std=c99 -Wall -Wextra -Werror -fsanitize=address,undefined -I"$H/src" "$H/tests/vehicle/test.c" \
 "$H/src/vcpe_path.c" "$H/src/vcpe_vehicle.c" "$H/src/vcpe_math.c" -lm -o "$O"
"$O"
