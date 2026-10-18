#!/bin/sh
# vcpe_sky CPU side. Host only.
set -eu
H=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd); V=$(CDPATH= cd -- "$H/../.." && pwd)
D=$(mktemp -d /tmp/vcpe-sky.XXXXXX); trap 'rm -rf "$D"' EXIT HUP INT TERM
${CC:-clang} -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I"$V/src" \
    "$H/test.c" "$V/src/vcpe_sky.c" -lm -o "$D/test"
"$D/test"
