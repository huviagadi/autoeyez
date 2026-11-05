#!/usr/bin/env python3
"""Reserve clip slot 36 and expose STATIC_VEIL at slot 40."""

import os
from pathlib import Path

import clip_catalog

OLD = '036_STATIC_VEIL.mp4'
RESERVED = '036_RESERVED.mp4'
NEW = '040_STATIC_VEIL.mp4'
DIRS = (Path('/home/admin/clips'), Path('/home/admin/clips_seek'))


def mutate(data):
    order = data['order']
    if len(order) == 40 and order[35] == RESERVED and order[39] == NEW:
        print('Already installed')
        return
    expected = ('035_KALEIDOSCOPE.mp4', OLD,
                '037_Ghost Story.mp4', '038_Haunted House.mp4',
                '039_Mad Doctor.mp4')
    if len(order) != 39 or tuple(order[34:39]) != expected:
        raise RuntimeError('Unexpected clip order; refusing to change slots')
    for directory in DIRS:
        if not (directory / OLD).is_file() or (directory / RESERVED).exists() or (directory / NEW).exists():
            raise RuntimeError('Unexpected files in ' + str(directory))

    changed = []
    try:
        for directory in DIRS:
            os.link(directory / OLD, directory / NEW)
            changed.append(('link', directory))
            os.rename(directory / OLD, directory / RESERVED)
            changed.append(('rename', directory))
        order[35] = RESERVED
        order.append(NEW)
        folder = data['assigned'].pop(OLD, 'CLASSICS')
        data['assigned'][RESERVED] = folder
        data['assigned'][NEW] = folder
    except Exception:
        for action, directory in reversed(changed):
            if action == 'rename':
                os.rename(directory / RESERVED, directory / OLD)
            else:
                os.unlink(directory / NEW)
        raise


if __name__ == '__main__':
    data = clip_catalog.update(mutator=mutate)
    print('Slots 35-40:', list(enumerate(data['order'][34:40], 35)))
