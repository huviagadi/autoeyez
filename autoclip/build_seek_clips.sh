#!/bin/bash
set -euo pipefail

SOURCE_DIR=${SOURCE_DIR:-/home/admin/clips}
OUTPUT_DIR=${OUTPUT_DIR:-/home/admin/clips_seek}

mkdir -p "$OUTPUT_DIR"

find "$SOURCE_DIR" -maxdepth 1 -type f -name '*.mp4' -print0 |
while IFS= read -r -d '' source; do
    name=$(basename "$source")
    output="$OUTPUT_DIR/$name"
    partial="$output.part.mp4"

    if [ -s "$output" ]; then
        echo "Already built: $name"
        continue
    fi

    echo "Building: $name"
    rm -f "$partial"
    nice -n 15 ffmpeg -nostdin -v error -stats -y -i "$source" -map 0:v:0 -an \
        -c:v libx264 -preset ultrafast -threads 2 -crf 18 \
        -g 15 -keyint_min 15 -sc_threshold 0 -pix_fmt yuv420p \
        -movflags +faststart "$partial"

    src_duration=$(ffprobe -v error -select_streams v:0 \
        -show_entries stream=duration -of default=nw=1:nk=1 "$source")
    out_duration=$(ffprobe -v error -select_streams v:0 \
        -show_entries stream=duration -of default=nw=1:nk=1 "$partial")
    python3 - "$src_duration" "$out_duration" <<'PY'
import sys
source, output = map(float, sys.argv[1:])
if abs(source - output) > 0.1:
    raise SystemExit('duration mismatch: source={} output={}'.format(source, output))
PY

    mv "$partial" "$output"
done
