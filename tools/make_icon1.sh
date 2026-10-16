#!/bin/sh
# ICON1.PMF (XMB animated icon) from 144x80 PNG frames f0000.png... in DIR at 29.97 fps.
# Usage: make_icon1.sh DIR    (writes DIR/ICON1.PMF)
# BITRATE (kbps, default 200): ICON1 + SND0 should stay under ~500 KB.
# Needs ffmpeg and x264 (e.g. nix shell nixpkgs#x264).
set -e
T=$(cd "$(dirname "$0")" && pwd)
cd "$1"
# 1. PNG frames to raw YUV 4:2:0, full range, BT.601 colours (what the PSP's decoder assumes).
ffmpeg -hide_banner -loglevel error -y -framerate 30000/1001 -i f%04d.png \
  -vf "scale=144:80:out_range=full:out_color_matrix=bt601,format=yuv420p" -f rawvideo icon.yuv
# 2. Two-pass H.264 with the limits the XMB's player accepts: main profile level 2.1, no B-frames, one reference
#    frame, capped bitrate.
for p in 1 2; do
  x264 --demuxer raw --input-csp i420 --input-res 144x80 --fps 30000/1001 --input-range pc --range pc \
    --profile main --level 2.1 --ref 1 --bframes 0 --weightp 0 --keyint 48 --min-keyint 8 --scenecut 40 \
    --vbv-maxrate 600 --vbv-bufsize 600 --nal-hrd vbr --aud --pic-struct --bitrate ${BITRATE:-200} \
    --pass $p --stats x264.stats -o icon.264 icon.yuv
done
# 3. Wrap the H.264 stream in the PMF container (mkicon1.py).
python3 "$T/mkicon1.py" icon.264 ICON1.PMF
