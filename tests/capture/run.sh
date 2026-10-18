#!/bin/sh
set -eu
H=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
V=$(CDPATH= cd -- "$H/../.." && pwd)
D=$(mktemp -d /tmp/vcpe-capture.XXXXXX)
trap 'rm -rf "$D"' EXIT HUP INT TERM
${CC:-cc} -O2 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections ${CFLAGS:-} \
 -I"$H/stubs" -I"$V/src" "$H/staging.c" -Wl,--gc-sections -o "$D/staging"
"$D/staging"
