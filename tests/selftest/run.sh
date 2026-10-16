#!/bin/sh
# Build and run the standalone engine regression scene. No game files are needed.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
ENGINE=${VCPE_ROOT:-$ROOT}
EMU=${PSP_EMU:-$(command -v PPSSPPHeadless || true)}
[ -n "$EMU" ] || { echo 'Set PSP_EMU to PPSSPPHeadless.' >&2; exit 1; }
mkdir -p "$HERE/build" "$HERE/out"
python3 - "$ENGINE/tools" "$HERE/build" <<'PYDATA'
import sys
from pathlib import Path
sys.path.insert(0, sys.argv[1])
from vcpe_pak import write_pak
from vcpe_lod import build_lod, uv_sphere
v, i = uv_sphere()
v = [(a,b,0xFF000000 | int(255*a)<<16 | int(255*b)<<8 | 0x60,x,y,z) for a,b,c,x,y,z in v]
build=Path(sys.argv[2])
write_pak(build/'GAME.PAK',build/'pakinfo.h',[('hello',b'hello vcpe'+bytes(54)),('lodsphere',build_lod(v,i)[0])])
PYDATA
rm -rf "$HERE/vcpe_obj" "$HERE"/src/*.o
podman run --rm --userns=keep-id -v "$HERE:/work:Z" -v "$ENGINE:/vcpe:z" -w /work \
    -e PSPSDK=/usr/local/pspdev/psp/sdk -e PATH=/usr/local/pspdev/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
    ghcr.io/pspdev/pspdev:latest make -j8 VCPE=/vcpe > "$HERE/out/build.log" 2>&1
D=$(mktemp -d /tmp/vcpe-selftest.XXXXXX)
trap 'rm -rf "$D"' EXIT HUP INT TERM
G=$D/PSP/GAME/T
mkdir -p "$G"
cp "$HERE/EBOOT.PBP" "$HERE/build/GAME.PAK" "$G/"
cat > $G/AUTOTEST.TXT <<'S'
wait 5
snap plain
batch 1
wait 2
snap batch
pipeline 1
wait 3
snap pipe
batch 0
wait 2
snap pipeplain
pipeline 0
press cross
wait 2
press circle
wait 2
hold triangle 5
press start
hold cross+circle 3
wait 2
report 0 5
scene 1
rec 1
wait 3
rec 0
snap perf
scene 2
mreset
mouse 100 100
mat 100 100
click 280 80
mat 280 80
wait 2
dclick 330 130
wait 2
rclick 330 130
drag 280 80 430 180 10
wait 2
mdrop 430 180
warp left
mat 380 180
warp up
mat 380 130
wheel 3
wheel -1
mouse 9999 -50
mat 479 0
mouse 100 100
warp right
mat 280 80
mdown
glide 282 80 4
mup
mcheck 3 1 1 1 1 2 3
drag 280 80 310 130 6
snap mouse
wait 2
report 0 5
scene 3
wait 3
snap lod
lodfull 1
wait 2
snap lodfull
lodfull 0
loading
wait 2
quit
S
timeout 60 "$EMU" --memstick="$D" --timeout-emulated=30 --graphics=software "$G/EBOOT.PBP" > "$HERE/out/run.log" 2>&1
cat "$G/SELFTEST.LOG"
python3 - "$G" "$HERE/out" <<'PYVERIFY'
from pathlib import Path
import sys
from PIL import Image
src,out=map(Path,sys.argv[1:])
log=(src/'SELFTEST.LOG').read_text()
run=(out/'run.log').read_text()
assert 'FAIL' not in log and 'TIMEOUT' not in run
assert 'RESULT PASS (0 failures)' in log
names=('plain','batch','pipe','pipeplain','perf','mouse','lod','lodfull','loading','loading2')
for name in names:
    data=(src/('SNAP'+name+'.RAW')).read_bytes()
    assert len(data)==480*272*4
    (out/(name+'.raw')).write_bytes(data)
    Image.frombytes('RGBA',(480,272),data).convert('RGB').save(out/(name+'.png'))
a=(out/'plain.raw').read_bytes()
for name in ('batch','pipe','pipeplain'):
    assert a==(out/(name+'.raw')).read_bytes(),name
    print('PASS snapshot '+name+' = plain')
a,b=[(out/(name+'.raw')).read_bytes() for name in ('lod','lodfull')]
d=[max(abs(a[i+k]-b[i+k]) for k in range(3)) for i in range(480*4*20,len(a),4)]
big=sum(v>48 for v in d); changed=sum(v>0 for v in d)
assert big<150 and changed-big<4000,(changed,big)
print('PASS LOD silhouette tolerance:',changed,'pixels differ;',big,'above 48 levels')
assert (out/'loading.raw').read_bytes()!=(out/'loading2.raw').read_bytes()
print('PASS loading animation moves')
PYVERIFY
