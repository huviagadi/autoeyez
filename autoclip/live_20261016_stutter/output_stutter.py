"""Immediate 30 fps stutter of the final mixed video frame."""

from collections import deque

import cv2


class OutputStutter:
    def __init__(self, max_frames=30):
        self.max_frames = max_frames
        self.history = deque(maxlen=max_frames)
        self.segment = []
        self.indices = {'normal': 0, 'reverse': 0}
        self.active = False

    def apply(self, live_frame, lengths):
        """Replay recent output while live sources continue advancing behind it."""
        lanes = {
            name: max(0, min(self.max_frames, int(lengths.get(name, 0))))
            for name in ('normal', 'reverse')
        }
        active = any(lanes.values())
        if active and not self.active:
            self.segment = list(self.history) or [live_frame.copy()]
            self.indices = {'normal': 0, 'reverse': 0}

        output = live_frame
        if active:
            heads = []
            for name, length in lanes.items():
                if not length:
                    continue
                window = self.segment[-length:]
                index = self.indices[name] % len(window)
                heads.append(window[index])
                self.indices[name] = (index + 1) % len(window)
            output = heads[0] if len(heads) == 1 else cv2.addWeighted(
                heads[0], 0.5, heads[1], 0.5, 0)
        elif self.active:
            self.segment = []

        # Keep collecting the current live mix during replay so release and a
        # subsequent press both jump to the latest clip and shader state.
        self.history.append(live_frame.copy())
        self.active = active
        return output
