#!/bin/sh
# Open-world modules (5.2): every host suite, with sanitizers. No game, emulator or console needed.
set -eu
H=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
for t in rig animator terrain cells collide sky crowd; do
    sh "$H/$t/run.sh"
done
echo 'PASS VCPE 5.2 open-world host suites'
