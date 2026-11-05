#!/usr/bin/env python3
"""Make reproducible H.264 access-unit/slice loss trials from Annex B input.

This is an offline experiment. It never touches the running stream.
"""

import re
import sys
from pathlib import Path


def parse_nals(data):
    starts = list(re.finditer(b"\x00\x00\x00?\x01", data))
    for index, match in enumerate(starts):
        end = starts[index + 1].start() if index + 1 < len(starts) else len(data)
        if match.end() < end:
            yield data[match.start():end], data[match.end()] & 0x1F


def access_units(data):
    units = []
    current = []
    for nal, kind in parse_nals(data):
        if kind == 9 and current:
            units.append(current)
            current = []
        current.append((nal, kind))
    if current:
        units.append(current)
    return units


def write_units(path, units):
    Path(path).write_bytes(b"".join(nal for unit in units for nal, _ in unit))


def main(source, out_dir):
    units = access_units(Path(source).read_bytes())
    if len(units) != 90:
        raise RuntimeError("Expected 90 access units, got {}".format(len(units)))
    if not any(kind == 5 for _, kind in units[45]):
        raise RuntimeError("Frame 45 is not the expected IDR")
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)

    # Classic reference break: suppress the keyframe at the scene cut.
    write_units(out / "drop_cut_idr.h264", units[:45] + units[46:])

    # Smaller failure: remove a single predicted slice after the cut.
    damaged = [[(nal, kind) for nal, kind in unit] for unit in units]
    p_slices = [i for i, (_, kind) in enumerate(damaged[52]) if kind == 1]
    if not p_slices:
        raise RuntimeError("Frame 52 has no predicted slice")
    del damaged[52][p_slices[len(p_slices) // 2]]
    write_units(out / "drop_one_p_slice.h264", damaged)

    print("90 input units; dropped cut IDR or one of {} P slices".format(
        len(p_slices)))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
