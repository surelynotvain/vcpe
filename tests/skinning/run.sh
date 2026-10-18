#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
${CC:-clang} -std=c99 -Wall -Wextra -Werror -fsanitize=address,undefined -I"$root/src" \
 "$root/tests/skinning/test.c" "$root/src/vcpe_skin.c" -lm -o "$out/test"
"$out/test"
