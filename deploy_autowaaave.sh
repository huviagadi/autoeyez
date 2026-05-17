#!/bin/bash
# Deploy ofApp.cpp to autowaaave Pi, rebuild, and restart service
# Usage: ./deploy_autowaaave.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LOCAL_SRC="$SCRIPT_DIR/autowaaave/ofApp.cpp"
REMOTE_HOST="admin@autowaaave.local"
REMOTE_DIR="/home/pi/openFrameworks/apps/myApps/AUTO_WAAAVE_4_5"

echo "Copying ofApp.cpp..."
scp "$LOCAL_SRC" "$REMOTE_HOST:/tmp/"

echo "Building and restarting..."
ssh "$REMOTE_HOST" "sudo cp /tmp/ofApp.cpp $REMOTE_DIR/src/ && \
    cd $REMOTE_DIR && sudo make -j4 && \
    sudo systemctl restart autowaaave"

echo "Done."
