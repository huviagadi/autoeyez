import numpy as np

from output_stutter import OutputStutter


def frame(value):
    return np.full((2, 2, 3), value, dtype=np.uint8)


def value(frame_array):
    return int(frame_array[0, 0, 0])


def test_immediate_loop_and_live_release():
    effect = OutputStutter(max_frames=4)
    for n in range(4):
        assert value(effect.apply(frame(n), {})) == n
    assert value(effect.apply(frame(4), {'normal': 2})) == 2
    assert value(effect.apply(frame(5), {'normal': 2})) == 3
    assert value(effect.apply(frame(6), {'normal': 2})) == 2
    assert value(effect.apply(frame(7), {})) == 7
    # The live history advanced behind the replay.
    assert value(effect.apply(frame(8), {'normal': 4})) == 4


def test_dual_lanes_and_empty_history():
    effect = OutputStutter(max_frames=4)
    assert value(effect.apply(frame(9), {'normal': 2})) == 9
    assert value(effect.apply(frame(10), {})) == 10
    assert value(effect.apply(frame(12), {})) == 12
    assert value(effect.apply(frame(14), {'normal': 1, 'reverse': 2})) == 11


def test_history_copies_reused_mixer_buffers():
    effect = OutputStutter(max_frames=2)
    shared = frame(4)
    effect.apply(shared, {})
    shared.fill(99)
    assert value(effect.apply(frame(5), {'normal': 1})) == 4


if __name__ == '__main__':
    test_immediate_loop_and_live_release()
    test_dual_lanes_and_empty_history()
    test_history_copies_reused_mixer_buffers()
