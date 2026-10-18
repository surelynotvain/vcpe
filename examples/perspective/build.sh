#!/bin/sh
set -eu
H=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
V=$(CDPATH= cd -- "$H/../.." && pwd)
python3 "$H/make_assets.py"
python3 - "$V" "$H" <<'PY'
from pathlib import Path
import sys
sys.path.insert(0,sys.argv[1]+'/tools');import mksfo
meta=mksfo.entries('VCPE Perspective Lite','1.03','UCJS19053',memsize=0,parental=1)
meta['PSP_SYSTEM_VER']='6.60'
mksfo.write(sys.argv[2]+'/PARAM.SFO',meta)
PY
podman run --rm --userns=keep-id -v "$V:/vcpe:z" -w /vcpe/examples/perspective \
 -e PSPSDK=/usr/local/pspdev/psp/sdk -e PATH=/usr/local/pspdev/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
 ghcr.io/pspdev/pspdev:latest sh -eu -c '
 make -B -o PARAM.SFO -j8 VCPE=/vcpe
 /usr/local/pspdev/bin/psp-nm vcpe_perspective_demo.elf > generated/symbols.txt
 '
python3 - "$H/generated/symbols.txt" <<'PY'
from pathlib import Path
import sys
symbols=Path(sys.argv[1]).read_text()
for forbidden in ('sceGuDepthBuffer','sceGum','vcpe_3d_','sceNetInet'):
    if forbidden in symbols: raise SystemExit('Unexpected import in flat demo: '+forbidden)
print('PASS flat demo imports: no depth setup, 3D matrices/renderer or Inet dependency')
PY
