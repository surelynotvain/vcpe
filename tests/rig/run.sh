#!/bin/sh
# vcpe_rig: blob validation, sampling, blending, IK and skinned meshes against Python references. Host only.
set -eu
H=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd); V=$(CDPATH= cd -- "$H/../.." && pwd)
D=$(mktemp -d /tmp/vcpe-rig.XXXXXX); trap 'rm -rf "$D"' EXIT HUP INT TERM
python3 "$H/make.py" "$D"
${CC:-clang} -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I"$V/src" \
    "$H/test.c" "$V/src/vcpe_rig.c" "$V/src/vcpe_skin.c" -lm -o "$D/test"
"$D/test" "$D"
