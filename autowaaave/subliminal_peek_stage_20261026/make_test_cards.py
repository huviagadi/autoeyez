"""Create neutral RGBA cards for checking the staged shutter effect."""

import struct
import zlib
from pathlib import Path


WIDTH, HEIGHT = 720, 480
OUT = Path(__file__).with_name("peek_images")
OUT.mkdir(exist_ok=True)


def chunk(kind, payload):
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload))


def write_png(path, pixels):
    raw = b"".join(b"\0" + bytes(row) for row in pixels)
    data = b"\x89PNG\r\n\x1a\n"
    data += chunk(b"IHDR", struct.pack(">2I5B", WIDTH, HEIGHT, 8, 6, 0, 0, 0))
    data += chunk(b"IDAT", zlib.compress(raw, 6))
    data += chunk(b"IEND", b"")
    path.write_bytes(data)


for index, color in enumerate(((255, 45, 95), (45, 240, 220), (250, 220, 50)), 1):
    pixels = []
    for y in range(HEIGHT):
        row = bytearray()
        for x in range(WIDTH):
            center = 95 < x < 625 and 80 < y < 400
            bar = center and ((x // 46 + index) % 3 != 0) and ((y // 34) % 4 != 0)
            ring = center and 80 < abs(x - 360) + abs(y - 240) < 115
            alpha = 255 if bar or ring else 0
            row.extend((*(color if alpha else (0, 0, 0)), alpha))
        pixels.append(row)
    write_png(OUT / f"test_{index:02d}.png", pixels)
