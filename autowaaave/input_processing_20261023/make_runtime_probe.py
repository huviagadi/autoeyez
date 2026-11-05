"""Write a short MIDI probe: enable all five input effects, then reset to zero."""
from pathlib import Path
import struct


def variable_length(value):
    result = [value & 0x7f]
    value >>= 7
    while value:
        result.insert(0, 0x80 | (value & 0x7f))
        value >>= 7
    return bytes(result)


track = bytearray()
for control in range(102, 107):
    track += b"\x00\xb0" + bytes((control, 24))
for index, control in enumerate(range(102, 107)):
    track += variable_length(3840 if index == 0 else 0) + b"\xb0" + bytes((control, 0))
track += b"\x00\xff\x2f\x00"
header = b"MThd" + struct.pack(">IHHH", 6, 0, 1, 480)
Path("/private/tmp/autoeyez_input_processing_probe.mid").write_bytes(
    header + b"MTrk" + struct.pack(">I", len(track)) + track
)
