#!/bin/bash
# deploy_to_pi5.sh — Deploy autowaaave files to Pi 5
# Run from Mac after Pi 5 is set up with openFrameworks

set -e

PI_HOST="${1:-pi@autowaaave.local}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PARENT_DIR="$(dirname "$SCRIPT_DIR")"

echo "Deploying autowaaave to $PI_HOST"
echo "========================================"

# Create directories on Pi
echo "Creating directories..."
ssh "$PI_HOST" "mkdir -p ~/openFrameworks/apps/myApps/AUTO_WAAAVE_4_5/src"
ssh "$PI_HOST" "mkdir -p ~/openFrameworks/apps/myApps/AUTO_WAAAVE_4_5/bin/data/shadersES2"

# Copy source files
echo "Copying source files..."
scp "$PARENT_DIR/ofApp.cpp" "$PI_HOST:~/openFrameworks/apps/myApps/AUTO_WAAAVE_4_5/src/"
scp "$PARENT_DIR/ofApp.h" "$PI_HOST:~/openFrameworks/apps/myApps/AUTO_WAAAVE_4_5/src/"

# Copy shaders
echo "Copying shaders..."
scp "$PARENT_DIR/shadersES2/"*.frag "$PI_HOST:~/openFrameworks/apps/myApps/AUTO_WAAAVE_4_5/bin/data/shadersES2/"
scp "$PARENT_DIR/shadersES2/"*.vert "$PI_HOST:~/openFrameworks/apps/myApps/AUTO_WAAAVE_4_5/bin/data/shadersES2/"

# Copy video_bridge.py
echo "Copying video_bridge.py..."
scp "$PARENT_DIR/video_bridge.py" "$PI_HOST:~/"

# Copy asound.conf
echo "Copying asound.conf..."
scp "$PARENT_DIR/asound.conf" "$PI_HOST:~/asound.conf.new"

# Copy service files
echo "Copying service files..."
scp "$SCRIPT_DIR/stream_receive_shm.service" "$PI_HOST:~/"
scp "$SCRIPT_DIR/autowaaave.service.pi5" "$PI_HOST:~/autowaaave.service"

echo ""
echo "========================================"
echo "Files deployed. Next steps on Pi:"
echo ""
echo "1. Apply shmsrc patch to ofGstVideoGrabber.cpp (see patch file)"
echo "2. Rebuild openFrameworks: cd ~/openFrameworks/scripts/linux && ./compileOF.sh -j4"
echo "3. Build app: cd ~/openFrameworks/apps/myApps/AUTO_WAAAVE_4_5 && make -j4"
echo "4. Install services:"
echo "   sudo cp ~/stream_receive_shm.service ~/autowaaave.service /etc/systemd/system/"
echo "   sudo cp ~/asound.conf.new /etc/asound.conf"
echo "   sudo systemctl daemon-reload"
echo "   sudo systemctl enable stream_receive_shm autowaaave"
echo "5. Reboot and test"
