#!/bin/sh
# SND0.AT3 (XMB background music): the first 30 s of a WAV as plain ATRAC3 66 kbps (LP4) with a
# hardware-safe fact count and a loop chunk (mksnd0.py).
# Usage: make_snd0.sh IN.wav OUT.at3
# Needs ffmpeg and atracdenc (github.com/dcherednik/atracdenc); ATRACDENC overrides the binary.
set -e
T=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d)
# 30 seconds with a short fade-in and a 2.5 s fade-out, a little quieter, 44.1 kHz stereo.
ffmpeg -hide_banner -loglevel error -y -i "$1" -t 30 \
  -af "afade=t=in:st=0:d=0.3,afade=t=out:st=27.5:d=2.5,volume=-2dB" -ar 44100 -ac 2 -c:a pcm_s16le "$W/snd0.wav"
# ATRAC3 at 66 kbps (LP4): the format the XMB plays.
${ATRACDENC:-atracdenc} -e atrac3_lp4 --container=riff -i "$W/snd0.wav" -o "$W/snd0_lp4.at3"
# 1323000 = 30 s x 44100 samples.
python3 "$T/mksnd0.py" "$W/snd0_lp4.at3" 1323000 "$2"
rm -rf "$W"
