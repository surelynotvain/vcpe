#!/bin/sh
# vcpe_cells with a fake loader, then with vcpe_pack over a generated PAK (one pack per cell). Host only.
set -eu
H=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd); V=$(CDPATH= cd -- "$H/../.." && pwd)
D=$(mktemp -d /tmp/vcpe-cells.XXXXXX); trap 'rm -rf "$D"' EXIT HUP INT TERM
ARGS=$(python3 - "$V/tools" "$D/cells.pak" <<'PY'
import sys
sys.path.insert(0, sys.argv[1])
from vcpe_packer import build_pack, KIND_TEXT
blob, args = bytearray(64), []
for z in range(8):
    for x in range(8):
        p = build_pack([('cell.txt', ('cell %d %d' % (x, z)).encode(), KIND_TEXT)])
        blob += b'\0' * (-len(blob) % 64)
        args += [x, z, len(blob), len(p)]
        blob += p
open(sys.argv[2], 'wb').write(blob)
print(' '.join(map(str, args)))
PY
)
${CC:-clang} -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I"$V/src" \
    "$H/test.c" "$V/src/vcpe_cells.c" "$V/src/vcpe_pack.c" -lm -o "$D/test"
"$D/test" "$D/cells.pak" $ARGS
