"""Strict video patch wire schema through version 7 (static codec control)."""

FIELDS = (
    [(f'enc{i}', 0, 127) for i in range(18)]
    + [(f'ctrl{i}', 0, 1) for i in range(8)]
    + [(f'func{i}', 0, 3) for i in range(8)]
    + [('react', -1, 2), ('lumaKey', 0, 127), ('lumaHigh', 0, 127),
       ('lumaLowEn', 0, 1), ('lumaHiEn', 0, 1),
       ('renderLumaInvert', 0, 1), ('renderMirror', 0, 1),
       ('mix', 0, 127), ('chA', 0, 27), ('chB', 0, 27),
       ('lumaSrc', 0, 28), ('lumaTrack', 0, 1), ('mixMode', 0, 2),
       ('clip', 0, 63), ('loop', 0, 1), ('srcHue', 0, 127),
       ('srcSat', 0, 127), ('srcVal', 0, 127), ('srcShader', 0, 127),
       ('mosh', 0, 127), ('chroma', 0, 127), ('crush', 0, 127),
       ('pixel', 0, 127), ('bleed', 0, 127), ('align', 0, 127),
       ('ghost', 0, 127), ('wobble', 0, 127), ('overdrive', 0, 127),
       ('hold', 0, 127), ('peek', 0, 127), ('peekMask', 0, 1),
       ('codecLevel', 0, 127), ('codecStatic', 0, 127), ('codecLow', 0, 127),
       ('codecMid', 0, 127), ('codecHigh', 0, 127), ('codecRate', 0, 127),
       ('houseCard', 0, 1), ('houseLogo', 0, 1), ('mainLogo', 0, 1),
       ('patchVersion', 1, 7), ('slot', 0, 63)]
)

ALL_KEYS = [name for name, _, _ in FIELDS]
V6_KEYS = [key for key in ALL_KEYS if key != 'codecStatic']
V5_KEYS = [key for key in V6_KEYS if key not in (
    'codecLevel', 'codecLow', 'codecMid', 'codecHigh', 'codecRate')]
LEGACY_KEYS = [key for key in V5_KEYS if key not in (
    'bleed', 'align', 'ghost', 'wobble', 'overdrive',
    'hold', 'peek', 'peekMask')]
V3_KEYS = [key for key in V5_KEYS if key not in ('hold', 'peek', 'peekMask')]
V4_KEYS = [key for key in V5_KEYS if key not in ('peek', 'peekMask')]


def video_patch(values, slot):
    if type(slot) is not int or not 0 <= slot < 64:
        raise ValueError('slot')
    if not isinstance(values, list) or len(values) < 2:
        raise ValueError('field count')
    version = values[-2]
    if type(version) is not int or not 1 <= version <= 7:
        raise ValueError('patch version')
    if len(values) == len(FIELDS):
        keys = ALL_KEYS
    elif len(values) == len(V6_KEYS) and version <= 6:
        keys = V6_KEYS
    elif len(values) == len(V5_KEYS) and version <= 5:
        keys = V5_KEYS
    elif len(values) == len(V4_KEYS) and version <= 4:
        keys = V4_KEYS
    elif len(values) == len(V3_KEYS) and version <= 3:
        keys = V3_KEYS
    else:
        raise ValueError('field count')
    patch = dict(zip(keys, values))
    for key in ALL_KEYS:
        if key not in patch:
            patch[key] = 0
    if version <= 5:
        patch.update(codecLevel=0, codecLow=100, codecMid=80,
                     codecHigh=80, codecRate=64)
    if version <= 6:
        patch['codecStatic'] = 0
    for key, low, high in FIELDS:
        value = patch[key]
        if type(value) is not int or not low <= value <= high:
            raise ValueError(key)
    if patch['slot'] != slot or patch['lumaKey'] > patch['lumaHigh']:
        raise ValueError('slot/threshold pair')
    return patch
