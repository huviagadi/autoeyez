"""Stable clip indices and virtual folder assignments for autoclip.

The player, controller bridge and folder manager all read this same catalog.
Existing filenames are never reordered; newly discovered MP4s append in FRESH.
"""
import fcntl
import json
import os
import tempfile
from pathlib import Path

CLIPS_DIR = Path(os.environ.get('AUTOEYEZ_CLIPS_DIR', '/home/admin/clips'))
CATALOG_PATH = Path(os.environ.get('AUTOEYEZ_CLIP_CATALOG',
                                   '/home/admin/.config/autoeyez/clip_catalog.json'))
MAX_CLIPS = 64
MAX_FOLDERS = 15


def _validate(data):
    if not isinstance(data, dict) or data.get('version') != 1:
        raise ValueError('clip catalog version')
    order, folders, assigned = (data.get(key) for key in ('order', 'folders', 'assigned'))
    if not isinstance(order, list) or len(order) > MAX_CLIPS or len(order) != len(set(order)):
        raise ValueError('clip catalog order')
    if any(not isinstance(name, str) or not name.endswith('.mp4') or '/' in name or not name for name in order):
        raise ValueError('clip catalog filename')
    if not isinstance(folders, list) or len(folders) < 2 or len(folders) > MAX_FOLDERS or folders[:2] != ['CLASSICS', 'FRESH'] or len(folders) != len(set(folders)):
        raise ValueError('clip catalog folders')
    if any(not isinstance(name, str) or not 1 <= len(name) <= 12 or any(c not in 'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_' for c in name) for name in folders):
        raise ValueError('clip catalog folder name')
    if not isinstance(assigned, dict) or any(name not in order or folder not in folders for name, folder in assigned.items()):
        raise ValueError('clip catalog assignments')
    return data


def _write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile('w', dir=path.parent, prefix='.clip_catalog.', delete=False) as stream:
        temp = Path(stream.name)
        json.dump(data, stream, indent=2)
        stream.write('\n')
        stream.flush()
        os.fsync(stream.fileno())
    try:
        os.replace(temp, path)
        fd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try: os.fsync(fd)
        finally: os.close(fd)
    finally:
        temp.unlink(missing_ok=True)


def update(mutator=None, path=CATALOG_PATH, clips_dir=CLIPS_DIR):
    """Read and synchronize the catalog under a cross-process lock."""
    path, clips_dir = Path(path), Path(clips_dir)
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(str(path) + '.lock', 'a+') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if path.exists():
            data = json.loads(path.read_text())
            before = json.dumps(data, sort_keys=True)
            # Migrate the first virtual-folder name without touching clip order.
            if isinstance(data.get('folders'), list) and data['folders'][:2] == ['LIBRARY', 'FRESH']:
                data['folders'][0] = 'CLASSICS'
                data['assigned'] = {clip: 'CLASSICS' if folder == 'LIBRARY' else folder
                                    for clip, folder in data['assigned'].items()}
            _validate(data)
        else:
            data = {'version': 1,
                    'order': sorted(p.name for p in clips_dir.iterdir() if p.is_file() and p.suffix.lower() == '.mp4') if clips_dir.exists() else [],
                    'folders': ['CLASSICS', 'FRESH'], 'assigned': {}}
            before = None
        current = sorted(p.name for p in clips_dir.iterdir() if p.is_file() and p.suffix.lower() == '.mp4') if clips_dir.exists() else []
        for name in current:
            if name not in data['order']:
                if len(data['order']) >= MAX_CLIPS: raise ValueError('clip catalog full (64)')
                data['order'].append(name)
                data['assigned'][name] = 'FRESH'
        if mutator is not None: mutator(data)
        _validate(data)
        if before is None or json.dumps(data, sort_keys=True) != before: _write(path, data)
        return data


def names(path=CATALOG_PATH, clips_dir=CLIPS_DIR):
    return update(path=path, clips_dir=clips_dir)['order']


def folder_message(data, display_names):
    """Encode folder metadata bound to the reliable clip-list checksum."""
    import zlib
    from clip_list_transfer import encode_list
    _, clip_crc = encode_list(display_names, '0000000000000000')
    folders = ','.join(data['folders'])
    assignments = ''.join(format(data['folders'].index(data['assigned'].get(name, 'CLASSICS')), 'x')
                          for name in data['order'])
    payload = folders + ':' + assignments
    folder_crc = '%08x' % (zlib.crc32(payload.encode('ascii')) & 0xffffffff)
    return 'FOL1:%s:%s:%s' % (clip_crc, folder_crc, payload)
