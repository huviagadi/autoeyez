#!/usr/bin/env python3
"""Restore the mixer state captured before the temporary H.264 trial."""

import socket
import time

commands = [
    "CH_A:0",
    "CH_B:17",
    "MIX:36",
    "LUMA_HIGH:56",
    "LUMA:34",
    "LUMA_SRC:18",
    "LUMA_HIGH_EN:1",
]

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
for command in commands:
    sock.sendto(command.encode(), ("127.0.0.1", 5006))
    time.sleep(0.08)
