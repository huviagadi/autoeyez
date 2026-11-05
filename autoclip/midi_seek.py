#!/usr/bin/env python3
"""
midi_seek.py — MIDI Seek Controller for autoclip
Part of the autoeyez video synthesis system

Listens for MIDI CC from external controller (Akai Force) via USB gadget
and sends seek commands to video_player.py for instant clip position jumps.

Uses raw MIDI device instead of ALSA sequencer for gadget compatibility.

MIDI Mapping:
    CC 1 (Mod Wheel): Seek position (0-127 → 0-100% of clip)

Usage:
    python3 midi_seek.py
"""

import json
import os
import sys

# IPC file to send seek commands to video_player.py
SEEK_FILE = '/tmp/player_seek.json'

# Raw MIDI device for USB gadget
MIDI_DEVICE = '/dev/snd/midiC0D0'

# MIDI CC for seek control
SEEK_CC = 1  # Mod wheel

def send_seek(position):
    """
    Send seek command to video_player.py via IPC file.

    Args:
        position: Float from 0.0 to 1.0
    """
    try:
        with open(SEEK_FILE, 'w') as f:
            json.dump({'position': position}, f)
    except:
        pass

def parse_midi_byte(status, data1, data2):
    """
    Parse a 3-byte MIDI message.

    Returns:
        Tuple of (message_type, channel, value1, value2) or None
    """
    msg_type = status & 0xF0
    channel = status & 0x0F

    if msg_type == 0xB0:  # Control Change
        return ('cc', channel, data1, data2)
    elif msg_type == 0x90:  # Note On
        return ('note_on', channel, data1, data2)
    elif msg_type == 0x80:  # Note Off
        return ('note_off', channel, data1, data2)
    return None

def main():
    print("MIDI Seek Controller starting...", flush=True)
    print("Using raw MIDI device: {}".format(MIDI_DEVICE), flush=True)

    if not os.path.exists(MIDI_DEVICE):
        print("Error: MIDI device not found: {}".format(MIDI_DEVICE), flush=True)
        sys.exit(1)

    print("Listening for CC {} (seek)...".format(SEEK_CC), flush=True)

    try:
        with open(MIDI_DEVICE, 'rb') as midi:
            while True:
                # Read MIDI bytes
                byte = midi.read(1)
                if not byte:
                    continue

                b = byte[0]

                # Check for status byte (high bit set)
                if b & 0x80:
                    status = b
                    # Read data bytes
                    data1 = midi.read(1)
                    data2 = midi.read(1)

                    if data1 and data2:
                        msg = parse_midi_byte(status, data1[0], data2[0])

                        if msg and msg[0] == 'cc' and msg[2] == SEEK_CC:
                            # CC 1 received
                            value = msg[3]
                            position = value / 127.0
                            send_seek(position)
                            print("Seek: {:.1f}% (CC {} = {})".format(
                                position * 100, SEEK_CC, value), flush=True)
                        elif msg:
                            # Log other messages for debugging
                            print("MIDI: {} ch={} d1={} d2={}".format(
                                msg[0], msg[1]+1, msg[2], msg[3]), flush=True)

    except KeyboardInterrupt:
        print("Shutting down...", flush=True)
    except Exception as e:
        print("Error: {}".format(e), flush=True)

if __name__ == '__main__':
    main()
