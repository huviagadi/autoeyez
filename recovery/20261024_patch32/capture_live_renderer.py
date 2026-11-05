#!/usr/bin/env python3
"""Capture current renderer state using prepare/abort only; never commit."""

import json
import uuid
from datetime import datetime, timezone
from pathlib import Path

from patch_schema import FIELDS
from patch_transport import owner_request

OUT = Path('/home/admin/renderer_recovery')
OUT.mkdir(parents=True, exist_ok=True)
STAMP = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')


def capture(payload):
    tx = uuid.uuid4().hex[:16]
    prepared = False
    try:
        result = owner_request('waaave', {
            'version': 1, 'tx': tx, 'phase': 'prepare', 'payload': payload,
        })
        prepared = True
        return result['before']
    finally:
        if prepared:
            owner_request('waaave', {'version': 1, 'tx': tx, 'phase': 'abort'})


patch = {key: low for key, low, _ in FIELDS}
patch.update(lumaHigh=127, patchVersion=4, slot=31)
video = capture({'kind': 'video', 'patch': patch})
audio = capture({'kind': 'audio_save', 'slot': 32})

video_path = OUT / ('renderer_' + STAMP + '.json')
audio_path = OUT / ('audio_' + STAMP + '.TXT')
video_path.write_text(json.dumps(video, indent=2, sort_keys=True) + '\n')
audio_path.write_text(audio['text'])
print(json.dumps({
    'video': str(video_path),
    'audio': str(audio_path),
    'renderer_fields': len(video['state']),
    'audio_bytes': len(audio['text']),
}))
