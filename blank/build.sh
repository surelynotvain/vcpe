#!/bin/sh
# Build the VCPE Blank Engine EBOOT (pspdev container). The picture comes from engineassets/staring.jpg:
# tools/mkblankimg.py rebuilds src/blankimg.h and ICON0.PNG when the picture changes.
set -e
H=$(cd "$(dirname "$0")" && pwd); V=$(cd "$H/.." && pwd)
[ "$H/src/blankimg.h" -nt "$V/engineassets/staring.jpg" ] 2>/dev/null || python3 "$V/tools/mkblankimg.py"
[ "$V/src/vcpe_loadimg.h" -nt "$V/engineassets/hetalia-czechia.gif" ] && \
    [ "$V/src/vcpe_loadimg.h" -nt "$V/tools/mkloadimg.py" ] || python3 "$V/tools/mkloadimg.py"
python3 - "$V/tools" "$H/PARAM.SFO" <<'PY'
import sys
sys.path.insert(0, sys.argv[1])
import mksfo
mksfo.write(sys.argv[2], mksfo.entries('VCPE 5.0 Blank Engine', '5.00', 'UCJS19050', memsize=1, parental=1))
PY
rm -rf "$H/vcpe_obj" "$H"/src/*.o
podman run --rm --userns=keep-id -v "$V:/vcpe:z" -w /vcpe/blank \
    -e PSPSDK=/usr/local/pspdev/psp/sdk -e PATH=/usr/local/pspdev/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
    ghcr.io/pspdev/pspdev:latest make -j8 VCPE=/vcpe
ls -la "$H/EBOOT.PBP"
