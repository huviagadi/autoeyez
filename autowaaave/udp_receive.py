#!/usr/bin/env python3
"""
udp_receive.py — Raw RGB UDP Receiver for autowaaave
Part of the autoeyez video synthesis system

Receives raw RGB frames from autoclip via UDP chunks, reassembles them,
and writes to v4l2loopback device for ofApp to capture.

Protocol:
    Each UDP packet contains:
    - Header (8 bytes): frame_num (u32) + chunk_idx (u16) + num_chunks (u16)
    - Payload: up to 60KB of raw RGB data

    Frames are reassembled from chunks and output when complete.
    Incomplete frames are dropped when a new frame starts.

Usage:
    python3 udp_receive.py

Systemd:
    Runs as udp_receive.service, replacing stream_receive.service
"""

import socket
import struct
import subprocess
import sys
import os

# =============================================================================
# CONFIGURATION
# =============================================================================

UDP_PORT = 1238              # Must match autoclip mixer.py UDP_DEST_PORT
W, H = 720, 576              # Frame resolution (must match autoclip)
FRAME_SIZE = W * H * 3       # RGB24 bytes per frame

# v4l2loopback output via ffmpeg
V4L2_DEVICE = '/dev/video10'

# =============================================================================
# FRAME ASSEMBLER
# =============================================================================

class FrameAssembler:
    """
    Reassembles UDP chunks into complete frames.

    Tracks chunks by frame number and outputs complete frames.
    Drops incomplete frames when a new frame starts.
    """

    def __init__(self):
        self.current_frame_num = None
        self.chunks = {}
        self.expected_chunks = 0

    def add_chunk(self, frame_num, chunk_idx, num_chunks, data):
        """
        Add a chunk and return complete frame if ready.

        Returns:
            bytes: Complete frame data if all chunks received, else None
        """
        # New frame - reset state
        if frame_num != self.current_frame_num:
            if self.current_frame_num is not None and len(self.chunks) > 0:
                # Previous frame was incomplete - dropped
                pass
            self.current_frame_num = frame_num
            self.chunks = {}
            self.expected_chunks = num_chunks

        # Store chunk
        self.chunks[chunk_idx] = data

        # Check if complete
        if len(self.chunks) == self.expected_chunks:
            # Reassemble in order
            frame_data = b''.join(self.chunks[i] for i in range(self.expected_chunks))
            # Reset for next frame
            self.chunks = {}
            return frame_data

        return None

# =============================================================================
# MAIN
# =============================================================================

def main():
    print("UDP Receiver starting on port {}".format(UDP_PORT), flush=True)

    # Create UDP socket
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)  # 4MB buffer
    sock.bind(('0.0.0.0', UDP_PORT))
    print("Socket bound to port {}".format(UDP_PORT), flush=True)

    # Start ffmpeg to write to v4l2loopback
    ffmpeg_cmd = [
        'ffmpeg', '-loglevel', 'warning',
        '-f', 'rawvideo',
        '-pix_fmt', 'rgb24',
        '-video_size', '{}x{}'.format(W, H),
        '-framerate', '30',
        '-i', 'pipe:0',
        '-pix_fmt', 'rgb24',
        '-f', 'v4l2', V4L2_DEVICE
    ]

    ffmpeg = subprocess.Popen(
        ffmpeg_cmd,
        stdin=subprocess.PIPE,
        stdout=subprocess.DEVNULL,
        stderr=sys.stderr
    )

    print("ffmpeg started, writing to {}".format(V4L2_DEVICE), flush=True)

    assembler = FrameAssembler()
    frame_count = 0

    packet_count = 0
    try:
        while True:
            # Receive packet (max ~60KB + header)
            packet, addr = sock.recvfrom(65535)
            packet_count += 1
            if packet_count % 1000 == 1:
                print("Packets received: {} (last {} bytes)".format(packet_count, len(packet)), flush=True)

            if len(packet) < 8:
                continue  # Invalid packet

            # Parse header
            frame_num, chunk_idx, num_chunks = struct.unpack('<IHH', packet[:8])
            data = packet[8:]

            # Add chunk and check for complete frame
            frame_data = assembler.add_chunk(frame_num, chunk_idx, num_chunks, data)

            if frame_data is not None:
                # Write complete frame to ffmpeg
                try:
                    ffmpeg.stdin.write(frame_data)
                    frame_count += 1
                    if frame_count % 300 == 0:  # Log every 10 seconds at 30fps
                        print("Frames received: {}".format(frame_count), flush=True)
                except BrokenPipeError:
                    print("ffmpeg pipe broken, restarting...", flush=True)
                    ffmpeg.kill()
                    ffmpeg = subprocess.Popen(
                        ffmpeg_cmd,
                        stdin=subprocess.PIPE,
                        stdout=subprocess.DEVNULL,
                        stderr=sys.stderr
                    )

    except KeyboardInterrupt:
        print("Shutting down...", flush=True)
    finally:
        ffmpeg.terminate()
        sock.close()

if __name__ == '__main__':
    main()
