#!/bin/sh
# vcpe_animator: layered playback, root motion and foot IK over a generated rig. Host only.
set -eu
H=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd); V=$(CDPATH= cd -- "$H/../.." && pwd)
D=$(mktemp -d /tmp/vcpe-animator.XXXXXX); trap 'rm -rf "$D"' EXIT HUP INT TERM
python3 "$H/make.py" "$D/rig.vrg"
${CC:-clang} -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I"$V/src" \
    "$H/test.c" "$V/src/vcpe_animator.c" "$V/src/vcpe_rig.c" "$V/src/vcpe_skin.c" -lm -o "$D/test"
"$D/test" "$D/rig.vrg"
