#!/bin/sh
# Reproducible native PSP traversal. No game assets or physical console needed.
set -eu
H=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
V=$(CDPATH= cd -- "$H/../.." && pwd)
PSP_EMU=${PSP_EMU:-$(command -v PPSSPPHeadless || true)}
[ -n "$PSP_EMU" ] || { echo 'Set PSP_EMU to your PPSSPPHeadless executable.' >&2;exit 1; }
OUT=${VCPE_FLAT_OUT:-$H/evidence}
mkdir -p "$OUT/PSP/GAME/FLATDEMO"
OUT=$(CDPATH= cd -- "$OUT" && pwd)
G=$OUT/PSP/GAME/FLATDEMO
cp "$H/EBOOT.PBP" "$G/EBOOT.PBP"
cp "$V/tests/perspective/navigation.txt" "$G/AUTOTEST.TXT"
touch "$G/TESTMODE.ON"
timeout 60 "$PSP_EMU" --memstick="$OUT" --timeout-emulated=30 --graphics=software "$G/EBOOT.PBP" > "$OUT/run.log" 2>&1
if rg -q 'FAIL|TIMEOUT|dropped [1-9]' "$OUT/run.log"; then cat "$OUT/run.log";exit 1;fi
[ "$(rg -c '^PASS' "$OUT/run.log")" = 6 ]
python3 - "$OUT" <<'PY'
from PIL import Image,ImageDraw
from pathlib import Path
import sys
out=Path(sys.argv[1]);game=out/'PSP/GAME/FLATDEMO'
names=('courtyard','hall','door','exit','side','floor')
board=Image.new('RGB',(960,876),(24,24,24));draw=ImageDraw.Draw(board)
for i,name in enumerate(names):
 raw=(game/('SNAP'+name+'.RAW')).read_bytes();assert len(raw)==480*272*4
 im=Image.frombytes('RGBA',(480,272),raw).convert('RGB');im.save(out/(name+'.png'))
 x=(i%2)*480;y=(i//2)*292;board.paste(im,(x,y+20));draw.text((x+8,y+4),name,fill='white')
board.save(out/'contact.png')
print('PASS six native PSP captures saved:',out)
PY
cat "$OUT/run.log"
