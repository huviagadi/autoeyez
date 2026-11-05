"""Repeat shader time at Force beat divisions selected by its CC envelope."""

from collections import deque
import json
import os
import socket
import statistics
import time


SOCKET_PATH = '/tmp/shader_beat_repeat.sock'
# MIDI clock supplies 24 ticks per quarter note. Triplets occupy two thirds
# of their straight-note duration; 1/64 spans one and a half clock ticks.
DIVISIONS = (
    ('1/4', 24.0),
    ('1/4T', 16.0),
    ('1/8', 12.0),
    ('1/8T', 8.0),
    ('1/16', 6.0),
    ('1/16T', 4.0),
    ('1/32', 3.0),
    ('1/64', 1.5),
)


def division_for_cc(raw, lane='normal'):
    """Select one of eight Force divisions; zero releases the repeat."""
    raw = max(0, min(127, int(raw)))
    if raw == 0:
        return None
    index = min(7, (raw - 1) * 8 // 127)
    # Keep the direction of the established frame-length controls: CC 22
    # grows from short to long; CC 24 shrinks from long to short.
    if lane == 'normal':
        index = 7 - index
    return DIVISIONS[index]


class ShaderBeatRepeat:
    def __init__(self, path=SOCKET_PATH):
        self.path = path
        if os.path.exists(path):
            os.unlink(path)
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.socket.bind(path)
        self.socket.setblocking(False)
        os.chmod(path, 0o666)
        self.tick_times = deque(maxlen=48)
        self.seconds_per_tick = 60.0 / (120.0 * 24.0)
        self.last_tick_ns = None
        self.tick_count = 0
        self.controls = {'normal': 0, 'reverse': 0}
        self.anchors = {}

    def poll(self):
        """Drain all pending MIDI events before rendering the next frame."""
        while True:
            try:
                data = self.socket.recv(512)
            except BlockingIOError:
                break
            try:
                message = json.loads(data)
                stamp = int(message['timestamp_ns'])
                if message.get('action') == 'clock':
                    if (self.last_tick_ns is not None and
                            stamp - self.last_tick_ns > 500_000_000):
                        self.tick_times.clear()
                        self.controls = {'normal': 0, 'reverse': 0}
                        self.anchors.clear()
                        self.tick_count = 0
                    if self.last_tick_ns is not None:
                        interval = (stamp - self.last_tick_ns) / 1e9
                        if 0.005 < interval < 0.1:
                            self.tick_times.append(interval)
                        self.tick_count += 1
                    self.last_tick_ns = stamp
                    if len(self.tick_times) >= 6:
                        self.seconds_per_tick = statistics.median(self.tick_times)
                elif message.get('action') == 'stutter_control':
                    lane = message.get('control')
                    if lane in self.controls:
                        self.controls[lane] = max(0, min(127, int(message['raw'])))
                elif message.get('action') == 'stop':
                    self.controls = {'normal': 0, 'reverse': 0}
                    self.anchors.clear()
                    self.last_tick_ns = None
                    self.tick_count = 0
                    self.tick_times.clear()
            except (KeyError, TypeError, ValueError, json.JSONDecodeError):
                continue

    def shader_elapsed(self, started, now=None):
        """Loop shader time at the selected clock-aligned musical division."""
        now = time.monotonic() if now is None else now
        elapsed = max(0.0, now - started)
        if not any(self.controls.values()):
            self.anchors.clear()
            return elapsed
        # An unplugged controller must not leave the shader permanently offset.
        if self.last_tick_ns is None or now - self.last_tick_ns / 1e9 > 0.5:
            self.anchors.clear()
            return elapsed
        lane = max(self.controls, key=lambda name: self.controls[name])
        _, division_ticks = division_for_cc(self.controls[lane], lane)
        tick_fraction = min(1.5, max(0.0,
            (now - self.last_tick_ns / 1e9) / self.seconds_per_tick))
        tick_position = self.tick_count + tick_fraction
        phase_seconds = (tick_position % division_ticks) * self.seconds_per_tick
        if started not in self.anchors:
            self.anchors[started] = elapsed - phase_seconds
        return max(0.0, self.anchors[started] + phase_seconds)

    def close(self):
        self.socket.close()
        if os.path.exists(self.path):
            os.unlink(self.path)
