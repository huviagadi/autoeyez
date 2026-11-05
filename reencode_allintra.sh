#!/bin/bash
# Re-encode all clips to HEVC all-intra for instant seeking
# Processes clips in autoclip/clips, outputs to autoclip/clips_allintra
# Then uploads to autoclip

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CLIPS_DIR="${CLIPS_DIR:-$SCRIPT_DIR/autoclip/clips}"
OUTPUT_DIR="${OUTPUT_DIR:-$SCRIPT_DIR/autoclip/clips_allintra}"
REMOTE="${REMOTE:-admin@autoclip.local}"
REMOTE_PATH="${REMOTE_PATH:-/home/admin/clips/}"

mkdir -p "$OUTPUT_DIR"

total=$(ls -1 "$CLIPS_DIR"/*.mp4 2>/dev/null | wc -l | tr -d ' ')
current=0

echo ""
echo "============================================================"
echo "  RE-ENCODING $total CLIPS TO HEVC ALL-INTRA"
echo "  Output: $OUTPUT_DIR"
echo "============================================================"
echo ""

for input in "$CLIPS_DIR"/*.mp4; do
    [ -e "$input" ] || continue
    current=$((current + 1))
    filename=$(basename "$input")
    output="$OUTPUT_DIR/$filename"

    echo "[$current/$total] $filename"

    # All-intra HEVC for instant seeking
    ffmpeg -y -i "$input" \
        -c:v hevc_videotoolbox \
        -allow_sw 1 \
        -g 1 \
        -b:v 40M \
        -tag:v hvc1 \
        -vf "scale=720:480,fps=30" \
        -an \
        "$output" 2>&1 | grep -E "frame=|time=|speed=" | tail -1

    if [ ${PIPESTATUS[0]} -ne 0 ]; then
        echo "  FAILED: $filename"
        continue
    fi

    old_size=$(du -h "$input" | cut -f1)
    new_size=$(du -h "$output" | cut -f1)
    echo "  Done: $old_size -> $new_size"
    echo ""
done

echo "============================================================"
echo "  RE-ENCODING COMPLETE"
echo "============================================================"
echo ""

# Show size comparison
old_total=$(du -sh "$CLIPS_DIR" | cut -f1)
new_total=$(du -sh "$OUTPUT_DIR" | cut -f1)
echo "Total size: $old_total -> $new_total"
echo ""

read -p "Upload all clips to autoclip? [y/N] " confirm
if [[ "$confirm" =~ ^[Yy]$ ]]; then
    echo ""
    echo "Uploading to $REMOTE:$REMOTE_PATH ..."

    for clip in "$OUTPUT_DIR"/*.mp4; do
        filename=$(basename "$clip")
        echo "  $filename"
        scp -q "$clip" "$REMOTE:~/"
        ssh "$REMOTE" "sudo mv ~/\"$filename\" \"$REMOTE_PATH\""
    done

    echo ""
    echo "Upload complete!"
fi
