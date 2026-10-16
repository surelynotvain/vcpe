#!/bin/sh
set -eu
H=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd); V=$(CDPATH= cd -- "$H/../.." && pwd)
D=$(mktemp -d /tmp/vcpe47.XXXXXX); trap 'rm -rf "$D"' EXIT HUP INT TERM
CC=${CC:-cc}
FLAGS="-O2 -g -Wall -Wextra -Wno-unused-parameter -ffunction-sections -fdata-sections ${CFLAGS:-}"
"$CC" $FLAGS -I"$H/stubs" -I"$V/tests/memory/stubs" -I"$V/src" "$H/runtime.c" -Wl,--gc-sections -lm -o "$D/runtime"
"$D/runtime"
"$CC" $FLAGS -I"$H/stubs" -I"$V/tests/memory/stubs" -I"$V/src" "$H/render.c" -Wl,--gc-sections -lm -o "$D/render"
"$D/render"
"$CC" $FLAGS -I"$V/src" "$H/loading.c" "$V/src/vcpe_prefetch.c" -o "$D/loading"
(cd "$D" && ./loading)
"$CC" $FLAGS -I"$V/src" "$H/diagnostics.c" "$V/src/vcpe_diag.c" "$V/src/vcpe_adapt.c" -o "$D/diagnostics"
"$D/diagnostics"
OFFSETS=$(python3 - "$V/tools" "$D/threaded.pak" <<'PY'
import sys
sys.path.insert(0,sys.argv[1])
from vcpe_packer import build_pack, KIND_TEXT
packs=[build_pack([('hello.txt',b'hello world',KIND_TEXT),('b.bin',bytes(100))]),
       build_pack([('x',b'x'*500)]),build_pack([('y',b'y'*900),('z',b'z')])]
blob=bytearray(64);offsets=[]
for pack in packs:
    blob+=bytes(-len(blob)%64);offsets.extend([len(blob),len(pack)]);blob+=pack
open(sys.argv[2],'wb').write(blob)
print(*offsets)
PY
)
"$CC" $FLAGS -pthread -I"$V/src" "$H/threaded_loading.c" "$V/src/vcpe_jobs.c" "$V/src/vcpe_pack.c" "$V/src/vcpe_ultra.c" -o "$D/threaded"
(cd "$D" && timeout 15 ./threaded $OFFSETS)
STB=${VCPE_STB_INCLUDE:-$D/sdk}
if [ ! -f "$STB/stb_truetype.h" ]; then
    mkdir -p "$STB"
    podman run --rm ghcr.io/pspdev/pspdev:latest cat /usr/local/pspdev/psp/include/stb_truetype.h > "$STB/stb_truetype.h"
fi
FONT=${VCPE_TEST_FONT:-$(fc-match -f '%{file}' sans)}
"$CC" $FLAGS -DFONT_NEW -I"$H/stubs" -I"$V/tests/memory/stubs" -I"$V/src" -I"$STB" "$H/font.c" -lm -o "$D/font"
"$D/font" "$FONT"
if [ "$#" -gt 0 ]; then
    BEFORE=$(CDPATH= cd -- "$1" && pwd)
    "$CC" $FLAGS "-DTTF_SOURCE=\"$BEFORE/src/vcpe_ttf.c\"" -I"$H/stubs" -I"$V/tests/memory/stubs" -I"$BEFORE/src" -I"$STB" "$H/font.c" -lm -o "$D/font-before"
    "$D/font-before" "$FONT" > "$D/font-before.txt"
    "$D/font" "$FONT" > "$D/font-after.txt"
    test "$(sed -n '1p' "$D/font-before.txt")" = "$(sed -n '1p' "$D/font-after.txt")"
    echo 'Font layout and rendered glyph coverage match the pre-4.7 engine exactly'
fi
