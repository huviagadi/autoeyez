"""ANGEL composition controls and migration from the first 12-value rig."""

DEFAULT_SHAPE = [
    38, 55, 75, 82, 64, 70, 80, 37,   # upper: root x/y, angle, span, curve, feathers, fan, flutter
    38, 73, 47, 73, 64, 62, 75, 43,   # lower: same
    64, 64, 64, 64, 86, 64, 75, 25,   # core: position x/y, scale, aspect, eyes, spacing, halos, palette
]
SHAPE_COUNT = len(DEFAULT_SHAPE)


def normalize_shape(value):
    if not isinstance(value, list) or any(type(v) is not int or not 0 <= v <= 127 for v in value):
        raise ValueError('invalid Angel shape')
    if len(value) == SHAPE_COUNT:
        return list(value)
    if len(value) == 12:
        # Preserve the earlier span, density, curve, flutter, core, and palette settings.
        result = list(DEFAULT_SHAPE)
        for old, new in enumerate((3, 5, 4, 7, 11, 13, 12, 15, 18, 20, 22, 23)):
            result[new] = value[old]
        return result
    raise ValueError('invalid Angel shape length')
