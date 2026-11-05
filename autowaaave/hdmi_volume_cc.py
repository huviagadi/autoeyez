#!/usr/bin/env python3
"""Receive Force CC46 and apply an independent HDMI playback gain offset."""

import socket
import subprocess
import time

SOURCE_IP = '10.0.0.2'
PORT = 5012
CONTROL = 'Autoeyez HDMI'


def offset_db(value):
    """63=unity; negative reaches -50 dB, positive reaches +6 dB."""
    if value <= 63:
        return (value - 63) * (50.0 / 63.0)
    return (value - 63) * (6.0 / 64.0)


def set_volume(value):
    db = offset_db(value)
    result = subprocess.run(
        ['amixer', '-q', '-c', 'vc4hdmi0', 'sset', CONTROL, '--',
         '{:.2f}dB'.format(db)],
        capture_output=True, text=True, timeout=2)
    if result.returncode:
        print('HDMI volume unavailable: {}'.format(result.stderr.strip()),
              flush=True)
    return result.returncode == 0


def main():
    receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    receiver.bind(('0.0.0.0', PORT))
    receiver.settimeout(0.1)
    desired = 63
    applied = None
    last_attempt = 0.0
    retry_seconds = 1.0
    while True:
        try:
            packet, sender = receiver.recvfrom(16)
        except socket.timeout:
            packet = None
            sender = None
        if (packet is not None and sender[0] == SOURCE_IP and len(packet) == 3
                and packet[0] & 0xf0 == 0xb0 and packet[1] == 46
                and packet[2] <= 127):
            desired = packet[2]
        now = time.monotonic()
        if desired != applied and now - last_attempt >= retry_seconds:
            last_attempt = now
            if set_volume(desired):
                applied = desired
                retry_seconds = 0.05
            else:
                retry_seconds = 1.0


if __name__ == '__main__':
    main()
