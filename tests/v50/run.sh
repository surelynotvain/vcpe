#!/bin/sh
# Engine-only release checks. Does not build, launch or update any demake or physical card.
set -eu
H=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
V=$(CDPATH= cd -- "$H/../.." && pwd)
OUT=${VCPE50_OUT:-$H/out}
mkdir -p "$OUT"
OUT=$(CDPATH= cd -- "$OUT" && pwd)
sh "$V/tests/memory/run.sh" > "$OUT/memory.log" 2>&1
sh "$V/tests/v47/run.sh" > "$OUT/v47.log" 2>&1
python3 "$V/tests/native-data/run.py" > "$OUT/data.log" 2>&1
python3 "$H/upgrade.py" > "$OUT/upgrade.log" 2>&1
cc -O2 -Wall -Wextra -Wno-deprecated-declarations -ffunction-sections -fdata-sections \
    -I"$V/tests/memory/stubs" -I"$V/src" "$H/volatile.c" "$V/src/vcpe_mem.c" \
    -Wl,--gc-sections -o "$OUT/volatile-test"
"$OUT/volatile-test" > "$OUT/volatile.log"
sh "$V/blank/build.sh" > "$OUT/build.log" 2>&1
if rg -q 'warning:|error:' "$OUT/build.log"; then
    echo "Blank build emitted diagnostics; inspect $OUT/build.log" >&2; exit 1
fi
EMU=${PSP_EMU:-$(command -v PPSSPPHeadless || true)}
[ -n "$EMU" ] || { echo 'Set PSP_EMU to run the PSP release checks.' >&2; exit 1; }
G=$OUT/PSP/GAME/VCPE50
mkdir -p "$G"
cp "$V/blank/EBOOT.PBP" "$G/EBOOT.PBP"
cat > "$G/AUTOTEST.TXT" <<'SCRIPT'
wait 5
bench
savecheck
wait 2
snap saves
loadpreview
wait 2
quit
SCRIPT
timeout 60 "$EMU" --memstick="$OUT" --timeout-emulated=30 --graphics=software "$G/EBOOT.PBP" > "$OUT/run.log" 2>&1
! rg -q 'FAIL|TIMEOUT' "$OUT/run.log"
test "$(rg -c '^PASS' "$OUT/run.log")" = 3
python3 - "$OUT" "$V/blank/ICON0.PNG" <<'PY'
from pathlib import Path
import struct,sys
from PIL import Image
out=Path(sys.argv[1]); game=out/'PSP/GAME/VCPE50'
for name in ('saves','loading0','loading1'):
    raw=(game/('SNAP'+name+'.RAW')).read_bytes()
    assert len(raw)==480*272*4
    Image.frombytes('RGBA',(480,272),raw).convert('RGB').save(out/(name+'.png'))
assert (game/'SNAPloading0.RAW').read_bytes() != (game/'SNAPloading1.RAW').read_bytes()
def sfo(path):
    data=path.read_bytes(); magic,version,ko,vo,count=struct.unpack_from('<4sIIII',data)
    assert magic==b'\0PSF'
    result={}
    for i in range(count):
        k,kind,n,cap,offset=struct.unpack_from('<HHIII',data,20+i*16)
        key=data[ko+k:].split(b'\0',1)[0].decode()
        result[key]=data[vo+offset:vo+offset+n].rstrip(b'\0')
    return result
for slot in ('DEMO00','IMPORT'):
    folder=out/'PSP/SAVEDATA'/('UCJS19050'+slot)
    meta=sfo(folder/'PARAM.SFO')
    assert meta['TITLE']==b'VCPE 5.0 Save Demo'
    assert meta['SAVEDATA_TITLE']==b'Engine test slot'
    assert b'VCPE 5.0' in meta['SAVEDATA_DETAIL']
    assert (folder/'DATA.BIN').stat().st_size>513
    assert (folder/'ICON0.PNG').read_bytes()==Path(sys.argv[2]).read_bytes()
assert (game/'MIGRATE.DAT').exists() or (game/'MIGRATE.DAT.BAK').exists()
print('PASS native savedata metadata/icons, legacy preservation and animated loader captures')
PY
cat "$OUT/upgrade.log" "$OUT/volatile.log" "$OUT/run.log"
echo "VCPE 5.0 engine checks passed. Evidence: $OUT"
