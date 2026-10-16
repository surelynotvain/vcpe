#!/bin/sh
# builds a PAK with 3 asset packs (tools/vcpe_packer.py), then runs the host test against it
set -eu
H=$(cd "$(dirname "$0")" && pwd); V=$(cd "$H/../.." && pwd)
D=$(mktemp -d /tmp/vcpe-jobs-pack.XXXXXX)
trap 'rm -rf "$D"' EXIT HUP INT TERM
OUT=$(python3 - "$V/tools" "$D/test.pak" <<'P'
import sys; sys.path.insert(0, sys.argv[1])
from vcpe_packer import build_pack, KIND_TEXT
packs = [build_pack([('hello.txt', b'hello world', KIND_TEXT), ('b.bin', bytes(100))]),
         build_pack([('x', b'x' * 500)]), build_pack([('y', b'y' * 900), ('z', b'z')])]
blob, offs = bytearray(64), []
for p in packs:
    blob += b'\0' * (-len(blob) % 64); offs += [len(blob), len(p)]; blob += p
open(sys.argv[2], 'wb').write(blob)
print(' '.join(map(str, offs)))
P
)
"${CC:-cc}" -O2 -Wall -Wextra -Wno-unused-parameter -pthread -I"$V/src" "$H/jobs_pack_test.c" "$V/src/vcpe_jobs.c" "$V/src/vcpe_pack.c" "$V/src/vcpe_ultra.c" -o "$D/test"
timeout 15 "$D/test" "$D/test.pak" $OUT
