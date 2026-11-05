"""Force Q-Link addresses and symmetric full-sweep offset conversion."""

OF_CC = frozenset((16, 17, 18, 19, 20, 21, 23, 25, 26, 27, 28,
                   29, 30, 31, 40, 41, 94, 95, 96, 97, 98, 122))
MIXER_CC = frozenset(set((42, 43, 44, 45)) |
                     set(range(50, 59)) |
                     set(range(60, 63)) |
                     set(range(65, 71)) |
                     set(range(72, 90)))
HDMI_VOLUME_CC = 46

SHADER_BASE_CC = ((50, 51, 52), (53, 54, 55), (56, 57, 58))
SHADER_BAND_CC = (
    ((60, 61, 62), (65, 66, 67), (68, 69, 70)),
    ((72, 73, 74), (75, 76, 77), (78, 79, 80)),
    ((81, 82, 83), (84, 85, 86), (87, 88, 89)),
)


def signed_offset(value):
    """0=-1, 63=0, 127=+1, matching the live OF Force layer."""
    if not isinstance(value, int) or not 0 <= value <= 127:
        raise ValueError('Force CC value')
    return (value - 63) / (63.0 if value <= 63 else 64.0)


def bounded(base, cc_value, span, low, high):
    """Add one physical sweep, then clamp to the destination's hard range."""
    return max(low, min(high, base + signed_offset(cc_value) * span))
