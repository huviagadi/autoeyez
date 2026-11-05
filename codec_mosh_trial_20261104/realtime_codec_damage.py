#!/usr/bin/env python3
"""Bounded Annex B H.264 damage proxy for the Autoeyez live stream.

Autoclip connects to port 1238. GStreamer listens locally on port 1239.
No decoded frames are queued here; each NAL is forwarded as soon as its end
start code arrives. The one-NAL parser delay is normally a fraction of a frame.
"""
import os
import socket
import threading
import time

FFT_FILE = '/dev/shm/fft_values'
CONTROL_FILE = '/dev/shm/codec_damage'
LISTEN = ('0.0.0.0', 1238)
OUTPUT = ('127.0.0.1', 1239)
MIDI_BANDS = (0, 0, 0)
MIDI_AT = 0.0


def receive_midi_bands():
    """Subscribe to the same ALSA sequencer source as the renderer."""
    global MIDI_BANDS, MIDI_AT
    import mido
    while True:
        try:
            ports = mido.get_input_names()
            name = next(name for name in ports if 'Teensy MIDI' in name)
            with mido.open_input(name) as port:
                print('Codec FFT subscribed to ' + name, flush=True)
                values = [0, 0, 0]
                extended = [0] * 6
                last_frame = time.monotonic()
                while True:
                    for message in port.iter_pending():
                        if message.type != 'control_change':
                            continue
                        if 110 <= message.control <= 112:
                            values[message.control - 110] = message.value
                        elif 114 <= message.control <= 119:
                            extended[message.control - 114] = message.value
                        elif message.control == 113:
                            # Coarse bass often clips at 127. The extended
                            # low band retains the headroom needed for hits.
                            # 14-bit payload spans 0..32 band units; its
                            # MIDI scaling is 512 counts per unit. Normalize
                            # to the same 0..127 scale as the coarse CC.
                            low = ((extended[0] << 7) | extended[1]) / 4.0
                            MIDI_BANDS = (low, values[1], values[2])
                            MIDI_AT = time.monotonic()
                            last_frame = MIDI_AT
                    # ALSA can leave a subscriber open but silent after a
                    # Teensy reflash/re-enumeration. FFT frames have a 100 ms
                    # heartbeat, so silence for a full second means reopen.
                    if time.monotonic() - last_frame > 1.0:
                        raise OSError('Teensy FFT MIDI heartbeat stopped')
                    time.sleep(0.005)
        except (OSError, StopIteration) as error:
            print('Codec FFT reconnect: %s' % error, flush=True)
            MIDI_AT = 0
            time.sleep(1)


def split_nals(buffer):
    """Return complete Annex B NALs and the unfinished tail."""
    starts = []
    offset = 0
    while True:
        at = buffer.find(b'\x00\x00\x01', offset)
        if at < 0:
            break
        if at > 0 and buffer[at - 1] == 0:
            at -= 1
        if not starts or at > starts[-1]:
            starts.append(at)
        offset = at + 4
    if len(starts) < 2:
        return [], buffer
    if starts[0]:
        buffer = buffer[starts[0]:]
        starts = [s - starts[0] for s in starts]
    return [buffer[starts[i]:starts[i + 1]] for i in range(len(starts)-1)], buffer[starts[-1]:]


def nal_type(nal):
    header = 4 if nal.startswith(b'\x00\x00\x00\x01') else 3
    return nal[header] & 31 if len(nal) > header else -1


class Damage:
    def __init__(self, clock=time.monotonic):
        self.clock = clock
        # audio, low, mid, high, rate, static
        self.controls = (0, 100, 80, 80, 64, 0)
        self.controls_mtime = -1
        self.fft_mtime = -1
        self.fft = (0, 0, 0)
        self.previous = (0, 0, 0)
        self.baseline = [0.0] * 3
        self.seen_fft = False
        self.armed = [True] * 3
        self.release_frames = [0] * 3
        self.trigger_peak = [0.0] * 3
        self.last_hit = [0.0] * 3
        self.pending = [False] * 3
        self.bass_idr_until = 0.0
        self.last_idr_drop = 0.0
        self.low_hits = 0
        self.low_burst = 0
        self.mid_burst = 0
        self.high_burst = 0
        self.frame = 0
        self.slices = 0
        self.drop_frame = False
        self.idr_frame = False
        self.have_clean_idr = False
        self.stats = [0, 0, 0]
        self.static_count = 0
        self.last_stats_write = 0.0

    def refresh(self):
        try:
            mtime = os.stat(CONTROL_FILE).st_mtime_ns
            if mtime != self.controls_mtime:
                values = tuple(map(int, open(CONTROL_FILE).read().split()))
                if len(values) in (5, 6) and all(0 <= v <= 127 for v in values):
                    self.controls = values if len(values) == 6 else values + (0,)
                self.controls_mtime = mtime
        except (OSError, ValueError):
            pass
        if self.clock() - MIDI_AT < 0.5:
            values = MIDI_BANDS
            stamp = MIDI_AT
            if stamp == self.fft_mtime:
                return
            self.fft_mtime = stamp
            self.accept_fft(values)
            return
        try:
            stat = os.stat(FFT_FILE)
            if self.clock() - stat.st_mtime > 0.5:
                self.fft = (0, 0, 0)
                self.previous = (0, 0, 0)
                self.pending = [False] * 3
                self.armed = [True] * 3
                self.baseline = [0.0] * 3
                self.seen_fft = False
                return
            if stat.st_mtime_ns == self.fft_mtime:
                return
            values = tuple(map(int, open(FFT_FILE).read().split()))
            self.fft_mtime = stat.st_mtime_ns
            if len(values) != 3:
                return
            self.accept_fft(values)
        except (OSError, ValueError):
            pass

    def accept_fft(self, values):
        self.fft = values
        if not self.seen_fft:
            self.baseline = list(values)
            self.previous = values
            self.seen_fft = True
            return
        level, low, mid, high, rate, static = self.controls
        gains = (low, mid, high)
        now = self.clock()
        for band in range(3):
            value = values[band]
            baseline = self.baseline[band]
            self.baseline[band] += (value - baseline) * (0.18 if value < baseline else 0.035)
            if not level or not gains[band]:
                self.pending[band] = False
                self.armed[band] = True
                continue
            strength = level * gains[band] / 127**2
            excursion = (8.3 - 3.8 * strength) if band == 0 else (
                (15 - 8 * strength) if band == 1 else (10 - 5.5 * strength))
            rise = (4.6 - 1.8 * strength) if band == 0 else (
                (8 - 3.2 * strength) if band == 1 else (4.5 - 1.8 * strength))
            floor = (8 if band == 0 else 10 if band == 1 else 6)
            if not self.armed[band]:
                self.trigger_peak[band] = max(self.trigger_peak[band], value)
                if value < max(floor, self.trigger_peak[band] * 0.45):
                    self.release_frames[band] += 1
                    if self.release_frames[band] >= 3:
                        self.armed[band] = True
                        self.release_frames[band] = 0
                else:
                    self.release_frames[band] = 0
                continue
            cooldown = 0.22 + (127 - rate) / 127 * 0.55
            if (value >= floor and value - baseline >= excursion and
                    value - self.previous[band] >= rise and
                    now - self.last_hit[band] >= cooldown):
                self.pending[band] = True
                self.armed[band] = False
                self.trigger_peak[band] = value
                self.last_hit[band] = now
        self.previous = values

    def process(self, nal):
        kind = nal_type(nal)
        if kind == 9:  # Access unit delimiter, emitted once per encoded frame.
            self.refresh()
            self.frame += 1
            self.slices = 0
            self.drop_frame = False
            self.idr_frame = False
            self.static_idr_damage = False
            self.static_this_frame = (self.frame * 73) % 127 < self.controls[5]
            self.static_start = (self.frame * 3) % 5
            self.static_width = (1 + self.controls[5] * 2 // 127 +
                                 2 * max(0, self.controls[5] - 95) // 32)
            self.static_did_drop = False
            now = self.clock()
            if now - self.last_stats_write >= 1.0:
                try:
                    with open('/dev/shm/codec_damage_stats', 'w') as output:
                        output.write('%d %d %d %d %d\n' %
                                     (self.frame, *self.stats, self.static_count))
                except OSError:
                    pass
                self.last_stats_write = now
            level, low, mid, high, rate, static = self.controls
            if level and self.pending[0]:
                self.pending[0] = False
                self.low_hits += 1
                self.stats[0] += 1
                self.low_burst = 1 + int(min(level, low) >= 112)
                # Deep reference breaks are reserved for the top of CD LOW;
                # regular bass hits make a smaller slice tear.
                chance = max(0, (low - 90) / 37) * level / 127
                if ((self.low_hits * 73) % 127) / 127 < chance:
                    self.bass_idr_until = now + 0.5
            if level and self.pending[1]:
                self.pending[1] = False
                self.mid_burst = max(self.mid_burst, 3 + mid // 43 +
                                     2 * int(min(level, mid) >= 112))
                self.stats[1] += 1
            if level and self.pending[2]:
                self.pending[2] = False
                self.high_burst = max(self.high_burst, 2 + high // 64 +
                                      2 * int(min(level, high) >= 112))
                self.stats[2] += 1
            # A burst is counted in predicted frames, not AUD markers; the
            # first slice decides the frame type below.
            return nal  # Keep AUD to preserve parser framing.
        if kind == 5 and self.slices == 0:
            self.idr_frame = True
            now = self.clock()
            # Seed the decoder with a clean keyframe. Later static keyframes
            # keep enough slices to produce a frame while carrying damage
            # through part of the image instead of leaving a playback gap.
            if self.have_clean_idr and self.controls[5] > 0:
                self.static_idr_damage = True
            elif (self.have_clean_idr and self.bass_idr_until > now and
                  now - self.last_idr_drop > 2.0):
                self.drop_frame = True
                self.last_idr_drop = now
                self.bass_idr_until = 0
            else:
                self.have_clean_idr = True
        if kind in (1, 5):
            self.slices += 1
            if self.drop_frame:
                return b''
            if (kind == 5 and self.static_idr_damage and self.slices in
                    (2 + self.static_start,)):
                return b''
            if kind == 1:
                if self.slices == 1:
                    self.low_slice = self.low_burst > 0
                    self.mid_this_frame = self.mid_burst > 0
                    self.high_this_frame = self.high_burst > 0
                    if self.low_burst:
                        self.low_burst -= 1
                    if self.mid_burst:
                        self.mid_burst -= 1
                    if self.high_burst:
                        self.high_burst -= 1
                level, low, mid, high, rate, static = self.controls
                low_slots = (3, 4, 5) if min(level, low) >= 112 else (4,)
                mid_slots = (2, 3, 4, 6) if min(level, mid) >= 112 else (2, 4, 6)
                high_slots = ((2, 3, 4, 5, 6) if min(level, high) >= 112 else
                              (2, 3, 5, 6) if high >= 96 else (3, 5))
                static_drop = (self.static_this_frame and 2 <= self.slices <= 6 and
                               (self.slices - 2 - self.static_start) % 5 < self.static_width)
                if static_drop and not self.static_did_drop:
                    self.static_count += 1
                    self.static_did_drop = True
                if (self.mid_this_frame and self.slices in mid_slots) or (
                        self.high_this_frame and self.slices in high_slots) or (
                        self.low_slice and self.slices in low_slots) or (
                        static_drop):
                    return b''
        return nal


def serve():
    threading.Thread(target=receive_midi_bands, daemon=True).start()
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(LISTEN)
    listener.listen(1)
    print('Codec damage proxy listening on %s:%d' % LISTEN, flush=True)
    while True:
        source, address = listener.accept()
        print('Sender connected: %s' % (address,), flush=True)
        with source:
            try:
                sink = socket.create_connection(OUTPUT, timeout=2)
            except OSError as e:
                print('Decoder unavailable: %s' % e, flush=True)
                continue
            with sink:
                source.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                sink.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                damage = Damage()
                tail = b''
                try:
                    while True:
                        chunk = source.recv(65536)
                        if not chunk:
                            break
                        units, tail = split_nals(tail + chunk)
                        if len(tail) > 2_000_000:
                            raise ValueError('Annex B start code missing')
                        for unit in units:
                            output = damage.process(unit)
                            if output:
                                sink.sendall(output)
                    if tail:
                        sink.sendall(damage.process(tail))
                except (OSError, ValueError) as e:
                    print('Stream ended: %s' % e, flush=True)
                print('Damage counts low/mid/high: %s' % damage.stats, flush=True)


if __name__ == '__main__':
    serve()
