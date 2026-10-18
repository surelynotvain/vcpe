#!/bin/sh
# vcpe_collide: rays and the character controller on built scenes. Host only.
set -eu
H=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd); V=$(CDPATH= cd -- "$H/../.." && pwd)
D=$(mktemp -d /tmp/vcpe-collide.XXXXXX); trap 'rm -rf "$D"' EXIT HUP INT TERM
${CC:-clang} -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -I"$V/src" \
    "$H/test.c" "$V/src/vcpe_collide.c" -lm -o "$D/test"
"$D/test"
