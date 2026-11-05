import uuid
from patch_schema import FIELDS, V5_KEYS
from patch_transport import owner_request

patch = {name: max(0, low) for name, low, high in FIELDS}
patch.update(lumaHigh=127, patchVersion=5, slot=0)
patch = {key: patch[key] for key in V5_KEYS}
tx = uuid.uuid4().hex[:16]
request = {'version': 1, 'tx': tx, 'phase': 'prepare',
           'payload': {'kind': 'video', 'patch': patch}}
try:
    result = owner_request('waaave', request)
    print('renderer accepted version-5 projection:', result['ok'], 'fields:', len(patch))
finally:
    owner_request('waaave', {'version': 1, 'tx': tx, 'phase': 'abort'})
