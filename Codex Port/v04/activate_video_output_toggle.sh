#!/bin/bash
set -euo pipefail

# Install the prepared bridge first. flash_teensy.sh restarts its service after
# programming, so the new firmware immediately has a VIDEO_OUT handler.
sudo install -m 0644 /home/admin/video_bridge.composite-ready.py /home/admin/video_bridge.py
HEX_FILE=/home/admin/auto_midi_video_output.hex /home/admin/flash_teensy.sh
/home/admin/enable_composite_output.sh
echo "Activation complete; rebooting autowaaave."
sudo reboot
