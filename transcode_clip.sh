#!/bin/bash
# Transcode clips for autoclip
# Usage: ./transcode_clip.sh file1.mp4 file2.mov ...
# Or drag files onto this script

# Local output folder - edit this path for your system
CLIPS_FOLDER="$HOME/clips/"
REMOTE="admin@autoclip.local"
REMOTE_PATH="/home/admin/clips/"
FOLDER_MANAGER="/home/admin/clip_folders.py"

total_files=$#
current_file=0

echo ""
echo "┌─────────────────────────────────────────────────────────────┐"
echo "│  CLIP TRANSCODER                                            │"
echo "│  Files to process: $total_files                                          │"
echo "└─────────────────────────────────────────────────────────────┘"
echo ""

mkdir -p "$CLIPS_FOLDER"

# The virtual folder is metadata on autoclip; media files stay in one directory.
folders=$(ssh -o ConnectTimeout=10 "$REMOTE" "python3 $FOLDER_MANAGER folders") || {
    echo "✗ Could not read autoclip folders"; exit 1;
}
echo "Available folders:"
echo "$folders"
folder="FRESH"
if [ -t 0 ]; then
    read -r -p "Folder for these clips [FRESH]: " choice
    [ -n "$choice" ] && folder=$(printf '%s' "$choice" | tr '[:lower:] ' '[:upper:]_')
fi
if ! printf '%s\n' "$folder" | grep -Eq '^[A-Z0-9_]{1,12}$'; then
    echo "✗ Folder names must use 1–12 letters, digits or underscores"; exit 1
fi
if ! printf '%s\n' "$folders" | grep -Fxq "$folder"; then
    if [ ! -t 0 ]; then echo "✗ Unknown folder: $folder"; exit 1; fi
    read -r -p "Create new folder $folder? [y/N]: " create_folder
    if [ "$create_folder" != "y" ] && [ "$create_folder" != "Y" ]; then exit 1; fi
    ssh -o ConnectTimeout=10 "$REMOTE" "python3 $FOLDER_MANAGER add $folder" || exit 1
fi

# Find next number
max=0
for f in "$CLIPS_FOLDER"*.mp4; do
    [ -e "$f" ] || continue
    base=$(basename "$f")
    num=$(echo "$base" | grep -oE '^[0-9]+' | sed 's/^0*//')
    [ -n "$num" ] && [ "$num" -gt "$max" ] && max=$num
done
next=$((max + 1))
remote_next=$(ssh -o ConnectTimeout=10 "$REMOTE" "python3 $FOLDER_MANAGER next") || exit 1
[ "$remote_next" -gt "$next" ] && next=$remote_next

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

    # All-intra HEVC for instant seeking (-g 1 = every frame is keyframe)
    # Uses Mac hardware encoder with software fallback
    ffmpeg -y -i "$input" \
        -c:v hevc_videotoolbox \
        -allow_sw 1 \
        -g 1 \
        -b:v 40M \
        -tag:v hvc1 \
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
    printf -v remote_name '%q' "$output_name"
    ssh -o ConnectTimeout=10 "$REMOTE" "sudo mv -- ~/$remote_name $REMOTE_PATH" || exit 1
    ssh -o ConnectTimeout=10 "$REMOTE" "python3 $FOLDER_MANAGER move $remote_name $folder" || exit 1
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
