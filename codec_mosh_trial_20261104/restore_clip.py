#!/usr/bin/env python3
"""Restore the clip and playback position captured before the trial."""

import json
import socket
import time

sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
target = "/tmp/player_seek.sock"
sock.sendto(json.dumps({"action": "play_index", "index": 23}).encode(), target)
time.sleep(0.15)
sock.sendto(json.dumps({"position": 54.64 / 134.64}).encode(), target)
