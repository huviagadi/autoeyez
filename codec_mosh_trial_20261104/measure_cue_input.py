#!/usr/bin/env python3
"""Time a controlled clip cue against frames arriving at the renderer input."""

import socket
import subprocess
import sys
import time

WIDTH = 720
HEIGHT = 480
FRAME_BYTES = WIDTH * HEIGHT * 3
SAMPLE_STEP = 256
TOTAL_FRAMES = 120
CUE_AFTER_FRAME = 25
CUE_INDEX = int(sys.argv[1]) if len(sys.argv) > 1 else 23

pipeline = [
    "gst-launch-1.0", "-q",
    "shmsrc", "socket-path=/tmp/video_shm", "is-live=true",
    "do-timestamp=true", "num-buffers={}".format(TOTAL_FRAMES),
    "!", "video/x-raw,format=RGB,width=720,height=480,framerate=30/1",
    "!", "fdsink", "fd=1", "sync=false",
]
proc = subprocess.Popen(pipeline, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

def read_exact(stream, n):
    data = bytearray(n)
    view = memoryview(data)
    offset = 0
    while offset < n:
        count = stream.readinto(view[offset:])
        if not count:
            raise EOFError("video stream ended after {} bytes".format(offset))
        offset += count
    return data

previous = None
cue_time = None
rows = []
for index in range(TOTAL_FRAMES):
    frame = read_exact(proc.stdout, FRAME_BYTES)
    arrival = time.monotonic()
    sampled = frame[::SAMPLE_STEP]
    difference = 0.0 if previous is None else sum(
        abs(a - b) for a, b in zip(sampled, previous)
    ) / len(sampled)
    previous = sampled
    if index == CUE_AFTER_FRAME:
        cue_time = time.monotonic()
        sock.sendto("PLAY:{}".format(CUE_INDEX).encode(), ("10.0.0.2", 5006))
    rows.append((index, arrival, difference))
proc.stdout.close()
proc.wait(timeout=5)

print("cue_index={} cue_at_frame={} cue_monotonic={:.6f}".format(
    CUE_INDEX, CUE_AFTER_FRAME, cue_time))
for index, arrival, difference in rows:
    if index >= CUE_AFTER_FRAME - 5 and index <= CUE_AFTER_FRAME + 45:
        print("frame={} elapsed_ms={:.1f} difference={:.2f}".format(
            index, (arrival - cue_time) * 1000, difference))
