#!/bin/bash
# Normalize a measured 60 Hz portrait framebuffer capture for landscape delivery.
set -euo pipefail
SOURCE=${1:?Pass the raw master MP4}
DEST=${2:?Pass a new delivery MP4 path}
START=${TRIM_START_SECONDS:-5}
[[ "$START" =~ ^[0-9]+([.][0-9]+)?$ ]] || { echo 'TRIM_START_SECONDS must be a nonnegative number.' >&2; exit 1; }
[ ! -e "$DEST" ] && [ ! -e "$DEST.encode.log" ] || { echo 'Output already exists.' >&2; exit 1; }
mkdir -p "$(dirname "$DEST")"
# Warm-up precedes the UI driver. A verified start can also omit test installation
# before app launch. Reset the origin before placing samples on the 60 Hz grid.
ffmpeg -hide_banner -loglevel verbose -nostdin -n -i "$SOURCE" \
  -vf "trim=start=$START,setpts=PTS-STARTPTS,transpose=2,fps=60" \
  -an -c:v libx264 -crf 18 -preset fast -pix_fmt yuv420p \
  -movflags +faststart "$DEST" > "$DEST.encode.log" 2>&1
SUMMARY=$(grep 'frames in,.*frames out;.*frames dropped,.*frames duplicated' "$DEST.encode.log" | tail -1)
printf '%s\n' "$SUMMARY"
if ! printf '%s\n' "$SUMMARY" | grep -q ', 0 frames duplicated'; then
  echo 'Cadence check failed: keep this output as a review copy, not accepted 60 fps.' >&2
  exit 1
fi
ffprobe -v error -select_streams v:0 \
  -show_entries stream=width,height,r_frame_rate,avg_frame_rate,nb_frames,duration \
  -of json "$DEST" > "$DEST.probe.json"
echo 'No export frames duplicated. Check source cadence, motion and orientation before acceptance.'
