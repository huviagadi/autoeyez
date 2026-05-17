#!/bin/bash
# Transcode clips for autoclip
# Usage: ./transcode_clip.sh file1.mp4 file2.mov ...
# Or drag files onto this script

# Local output folder - edit this path for your system
CLIPS_FOLDER="$HOME/clips/"
REMOTE="admin@autoclip.local"
REMOTE_PATH="/home/admin/clips/"

total_files=$#
current_file=0

echo ""
echo "┌─────────────────────────────────────────────────────────────┐"
echo "│  CLIP TRANSCODER                                            │"
echo "│  Files to process: $total_files                                          │"
echo "└─────────────────────────────────────────────────────────────┘"
echo ""

mkdir -p "$CLIPS_FOLDER"

# Find next number
max=0
for f in "$CLIPS_FOLDER"*.mp4; do
    [ -e "$f" ] || continue
    base=$(basename "$f")
    num=$(echo "$base" | grep -oE '^[0-9]+' | sed 's/^0*//')
    [ -n "$num" ] && [ "$num" -gt "$max" ] && max=$num
done
next=$((max + 1))

for input in "$@"; do
    current_file=$((current_file + 1))
    base=$(basename "$input" | sed 's/\.[^.]*$//')
    num_str=$(printf '%03d' $next)
    output_name="${num_str}_${base}.mp4"
    output_path="$CLIPS_FOLDER$output_name"

    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo "[$current_file/$total_files] $output_name"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

    echo ""
    echo "▶ STAGE 1/3: TRANSCODING..."
    echo "  Input:  $input"
    echo "  Output: $output_path"
    echo ""

    ffmpeg -y -i "$input" \
        -c:v libx264 \
        -preset ultrafast \
        -crf 20 \
        -vf "scale=720:480,fps=30" \
        -an \
        "$output_path" 2>&1 | grep -E "frame=|time=|speed="

    if [ ${PIPESTATUS[0]} -ne 0 ]; then
        echo ""
        echo "✗ FAILED: FFmpeg error on $input"
        read -p 'Press enter to close...'
        exit 1
    fi
    echo "✓ Transcode complete"
    echo ""

    echo "▶ STAGE 2/3: UPLOADING TO AUTOCLIP..."
    scp -o ConnectTimeout=10 "$output_path" "$REMOTE:~/" 2>&1
    if [ $? -ne 0 ]; then
        echo "✗ FAILED: Could not upload to $REMOTE"
        read -p 'Press enter to close...'
        exit 1
    fi
    echo "✓ Upload complete"
    echo ""

    echo "▶ STAGE 3/3: MOVING TO CLIPS FOLDER..."
    ssh -o ConnectTimeout=10 "$REMOTE" "sudo mv ~/$output_name $REMOTE_PATH 2>/dev/null; true"
    echo "✓ File moved to $REMOTE_PATH"
    echo ""

    echo "✓ DONE: $output_name"
    echo ""
    next=$((next + 1))
done

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "✓✓✓ ALL $total_files CLIPS COMPLETE! ✓✓✓"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""
read -p 'Press enter to close...'
