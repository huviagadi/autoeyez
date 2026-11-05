import hashlib
import json
import tempfile

import patch_coordinator as coordinator
from patch_schema import ALL_KEYS, FIELDS, V5_KEYS, V6_KEYS

calls = []
acks = []


def owner(name, request):
    calls.append((name, request))
    return {'before': {}}


def remote(host, name, request):
    return owner(name, request)


coordinator.owner_request = owner
coordinator.remote_request = remote
with tempfile.TemporaryDirectory() as journal:
    c = coordinator.PatchCoordinator(acks.append, journal=journal)
    for version, identity_keys in ((5, V5_KEYS), (6, V6_KEYS), (7, ALL_KEYS)):
        patch = {name: max(0, low) for name, low, high in FIELDS}
        patch.update(lumaHigh=127, patchVersion=version, slot=4,
                     codecLevel=100, codecStatic=50)
        # The wire carries the current normalized field set for old patches.
        tx = '%016x' % version
        c.receive('PTX:' + tx + ':VLOAD:4:' + ','.join(str(patch[k]) for k in ALL_KEYS))
        c.queue.join()
        prepared = [(target, req['payload']) for target, req in calls
                    if req['tx'] == tx and req['phase'] == 'prepare']
        render = next(payload['patch'] for target, payload in prepared if target == 'waaave')
        assert set(render) == set(V5_KEYS) and render['patchVersion'] == 5
        assert 'codecLevel' not in render and 'codecStatic' not in render
        expected = dict(patch)
        if version <= 5:
            expected.update(codecLevel=0, codecLow=100, codecMid=80,
                            codecHigh=80, codecRate=64)
        if version <= 6:
            expected['codecStatic'] = 0
        identity = {key: expected[key] for key in identity_keys}
        digest = hashlib.sha256(json.dumps(identity, sort_keys=True,
                                           separators=(',', ':')).encode()).hexdigest()
        cues = next(payload for target, payload in prepared if target == 'cues')
        assert cues['patch_identity'] == digest
        assert acks[-1] == 'PTX_ACK:' + tx + ':OK'
        print('version', version, 'renderer projection and cue identity OK')
