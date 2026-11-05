"""Source-independent, role-specific shader values stored with audio patches."""

from copy import deepcopy

ROLES = ('A', 'LUMA', 'B')
NEUTRAL = 64
CONTROL_LABELS = {
    5: ('WAVE SPEED', 'SQUIRCLE SIZE', 'COLOR PHASE'),
    6: ('ORBIT SPEED', 'LINE DENSITY', 'CONTRAST'),
    7: ('PULSE SPEED', 'PATTERN SCALE', 'COVERAGE'),
    8: ('RIPPLE SPEED', 'CELL DENSITY', 'RING WIDTH'),
    9: ('DRIFT SPEED', 'PATTERN SCALE', 'COLOR BALANCE'),
    11: ('DRIFT SPEED', 'PATTERN SCALE', 'COLOR BALANCE'),
    12: ('WAVE SPEED', 'TILE DENSITY', 'CIRCLE RADIUS'),
    13: ('DEPTH SPEED', 'CELL SCALE', 'COLOR PHASE'),
    14: ('SLICE SPEED', 'PATTERN SCALE', 'CONTRAST'),
    15: ('ZOOM SPEED', 'VIEW SCALE', 'PALETTE PHASE'),
    16: ('SEQUENCE SPEED', 'HEX SCALE', 'LINE WIDTH'),
    17: ('ROTATE SPEED', 'TILE DENSITY', 'EDGE WIDTH'),
    18: ('WAVE SPEED', 'WAVE SCALE', 'STRIPE DENSITY'),
    19: ('DANCE SPEED', 'RING SPACING', 'COLOR BALANCE'),
    20: ('TRAVEL SPEED', 'TILE DENSITY', 'TRI THRESHOLD'),
    21: ('SPIRAL SPEED', 'FIELD ZOOM', 'COLOR WAVE'),
    22: ('CYCLE SPEED', 'LINE SPACING', 'DISPLACEMENT'),
    23: ('PHASE SPEED', 'CELL SCALE', 'EDGE CONTRAST'),
}


def default_patch():
    return {
        'version': 1,
        'offsets': [[NEUTRAL] * 3 for _ in ROLES],
        'reactivity': [[[NEUTRAL] * 3 for _ in range(3)] for _ in ROLES],
    }


def validate_patch(value):
    if not isinstance(value, dict) or value.get('version') != 1:
        raise ValueError('shader patch version')
    offsets, reactivity = value.get('offsets'), value.get('reactivity')
    if not isinstance(offsets, list) or len(offsets) != 3:
        raise ValueError('shader offsets')
    if not isinstance(reactivity, list) or len(reactivity) != 3:
        raise ValueError('shader reactivity')
    def row(values):
        if not isinstance(values, list) or len(values) != 3 or any(
                type(item) is not int or item < 0 or item > 127 for item in values):
            raise ValueError('shader control value')
        return list(values)
    cleaned_reactivity = []
    for role in reactivity:
        if not isinstance(role, list) or len(role) != 3:
            raise ValueError('shader band count')
        cleaned_reactivity.append([row(values) for values in role])
    return {
        'version': 1,
        'offsets': [row(values) for values in offsets],
        'reactivity': cleaned_reactivity,
    }


def effective_values(patch, role, bands):
    """Normalized 0..1 slots, with signed contributions from each FFT band."""
    if role not in range(3) or len(bands) != 3:
        raise ValueError('shader role/bands')
    result = []
    for slot in range(3):
        value = patch['offsets'][role][slot]
        for band in range(3):
            value += (patch['reactivity'][role][band][slot] - NEUTRAL) * max(
                0.0, min(1.0, float(bands[band])))
        result.append(max(0.0, min(1.0, value / 127.0)))
    return tuple(result)


class ShaderControls:
    def __init__(self, patch=None):
        self.patch = validate_patch(patch) if patch is not None else default_patch()
        self.role = 0
        self.band = -1

    def select_role(self, role):
        if role not in range(3):
            raise ValueError('shader role')
        self.role = role

    def select_band(self, band):
        if band not in (-1, 0, 1, 2):
            raise ValueError('shader band')
        self.band = band

    def turn(self, slot, step):
        if slot not in range(3) or type(step) is not int:
            raise ValueError('shader slot/step')
        row = (self.patch['offsets'][self.role] if self.band < 0 else
               self.patch['reactivity'][self.role][self.band])
        row[slot] = max(0, min(127, row[slot] + step))
        return row[slot]

    def snapshot(self):
        return deepcopy(self.patch)

    def load(self, patch):
        self.patch = validate_patch(patch)
