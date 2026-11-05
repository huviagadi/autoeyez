#!/bin/bash
# Remote half of the Teensy flash workflow. Runs on autowaaave.
set -euo pipefail

HEX_FILE="${HEX_FILE:-/home/admin/auto_midi_latest.hex}"
LOADER="${LOADER:-/usr/bin/teensy_loader_cli}"

test -s "$HEX_FILE"
test -x "$LOADER"

# video_bridge translates this UDP request into the firmware's one-byte R
# command. The bridge must remain active until the request has been delivered.
python3 - <<'PY'
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.sendto(b'REBOOT', ('127.0.0.1', 5007))
PY

sleep 2
sudo systemctl stop video_bridge.service

cleanup() {
    sudo systemctl start video_bridge.service
}
trap cleanup EXIT

if ! sudo "$LOADER" --mcu=TEENSY41 -w -v "$HEX_FILE"; then
    # Debian's 2.2 loader occasionally fails its first write just after the
    # software reboot. HalfKay remains active, and the immediate retry works.
    sleep 1
    sudo "$LOADER" --mcu=TEENSY41 -v "$HEX_FILE"
fi
