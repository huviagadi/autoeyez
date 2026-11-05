#!/usr/bin/env python3
"""Forward Akai Force USB gadget CCs to autowaaave's separate visual layer."""

import os
import socket
import time
import math
import json

import mido
from command_ipc import CommandSender, MIXER_COMMAND_SOCKET
from force_offset_map import OF_CC, MIXER_CC, HDMI_VOLUME_CC


MIDI_PORT_MATCH = os.environ.get('AUTOEYEZ_FORCE_MIDI_PORT', 'f_midi:')
DESTINATION = (os.environ.get('AUTOEYEZ_FORCE_DEST_HOST', '10.0.0.1'), 5010)
BEAT_DESTINATION = (os.environ.get('AUTOEYEZ_FORCE_DEST_HOST', '10.0.0.1'), 5005)
HDMI_VOLUME_DESTINATION = (os.environ.get('AUTOEYEZ_FORCE_DEST_HOST', '10.0.0.1'), 5012)
PLAYER_SOCKET = os.environ.get('AUTOEYEZ_PLAYER_SOCKET', '/tmp/player_seek.sock')
SHADER_BEAT_SOCKET = os.environ.get('AUTOEYEZ_SHADER_BEAT_SOCKET',
                                   '/tmp/shader_beat_repeat.sock')
CLIPS_DIR = os.environ.get('AUTOEYEZ_CLIPS_DIR', '/home/admin/clips')
NOTE_MIN = 0
NOTE_MAX = 120
NOTE_RANGE = NOTE_MAX - NOTE_MIN + 1
AUDIO_PATCH_CUE_NOTE = 121  # C#8 on Force.
AUDIO_PATCH_CC = 93
AUDIO_PATCH_MIN = 1
AUDIO_PATCH_MAX = 32
VIDEO_PATCH_CUE_NOTE = 122  # D8 on Force; reserved for future video-patch load.
INPUT_TRIM_CUE_NOTE = 123  # D#8 on Force.
INPUT_TRIM_CC = 98
CLIP_CUE_NOTE = 127  # G8 in the Akai Force piano roll (C-2 = MIDI 0).
CLIP_CUE_MIN = 1
CLIP_CUE_MAX = 64
RESERVED_CLIP_CUES = frozenset((36,))  # Force velocity lane skips 36; keep its legacy patch slot.
PIXEL_CUE_NOTE = 126  # F#8 on Force.
PIXEL_CC = 97
PIXEL_CUE_LERP_MIN = 0.05
PIXEL_CUE_LERP_MAX = 0.5
STUTTER_CC = 22
STUTTER_REVERSE_CC = 24
SMOOTH_CC = OF_CC | MIXER_CC | {HDMI_VOLUME_CC}
SMOOTH_SECONDS = 0.045  # About 90% of a sudden change in 0.10 seconds.
REASSERT_SECONDS = 0.25  # Repair a missed UDP update without waiting for a Q-Link move.
STUTTER_MAX_FRAMES = 30
STUTTER_MIN_FRAMES = 2


class ForceBeatCounter:
    """Count quarter notes only after a transport or song-position anchor."""

    def __init__(self):
        self.tick = 0
        self.beat = 0
        self.running = False
        self.synced = False

    def update(self, message_type, position=None):
        if message_type == 'start':
            self.tick = 0
            self.beat = 0
            self.running = True
            self.synced = True
        elif message_type == 'songpos':
            # MIDI Song Position Pointer is measured in six-clock 16th notes.
            # At a quarter boundary, the next clock announces that beat.
            self.tick = (position % 4) * 6
            self.beat = ((position + 3) // 4) % 4
            self.synced = True
        elif message_type == 'continue':
            self.running = True
        elif message_type == 'stop':
            self.running = False
            return 'BEAT_STOP'
        elif message_type == 'clock':
            if not self.running or not self.synced:
                return None
            event = None
            if self.tick == 0:
                self.beat = self.beat % 4 + 1
                event = 'BEAT:{}'.format(self.beat)
            self.tick = (self.tick + 1) % 24
            return event
        return None


def scale_stutter(value):
    """Map MIDI 1-127 to 2-30 frames; preserve zero as release."""
    if value <= 0:
        return 0
    return STUTTER_MIN_FRAMES + round(
        (value - 1) * (STUTTER_MAX_FRAMES - STUTTER_MIN_FRAMES) / 126.0)


def scale_stutter_reverse(value):
    """Map MIDI 1-127 to 30-2 frames; preserve zero as release."""
    if value <= 0:
        return 0
    return STUTTER_MAX_FRAMES + STUTTER_MIN_FRAMES - scale_stutter(value)


def find_force_port():
    return next((name for name in mido.get_input_names()
                 if MIDI_PORT_MATCH in name), None)


def note_to_position(note, velocity):
    if note < NOTE_MIN or note > NOTE_MAX:
        return None
    note_pos = (note - NOTE_MIN) / NOTE_RANGE
    vel_offset = (velocity / 127.0) / NOTE_RANGE
    return min(1.0, note_pos + vel_offset)


def pixel_cue_lerp(current, target):
    """Ease large pixel jumps gently, then converge faster near the target."""
    distance = min(1.0, abs(target - current) / 127.0)
    proximity = 1.0 - distance
    curve = proximity * proximity * (3.0 - 2.0 * proximity)
    return PIXEL_CUE_LERP_MIN + (
        PIXEL_CUE_LERP_MAX - PIXEL_CUE_LERP_MIN) * curve


def main():
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    player = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    mixer_sender = CommandSender(MIXER_COMMAND_SOCKET, 'mixer')
    event_sequence = 0

    def event_fields():
        nonlocal event_sequence
        event_sequence += 1
        return {'event_id': '{}-{}'.format(os.getpid(), event_sequence),
                'sent_ns': time.monotonic_ns(), 'sent_wall_ns': time.time_ns()}

    def send(channel, control, value):
        if control in MIXER_CC:
            mixer_sender.send({'action': 'force_offset', 'cc': control,
                               'value': value})
        elif control == HDMI_VOLUME_CC:
            udp.sendto(bytes((0xb0 | channel, control, value)),
                       HDMI_VOLUME_DESTINATION)
        else:
            udp.sendto(bytes((0xb0 | channel, control, value)), DESTINATION)

    def send_seek(note, velocity):
        position = note_to_position(note, velocity)
        if position is None:
            return
        try:
            payload = dict(position=position, **event_fields())
            player.sendto(json.dumps(payload, separators=(',', ':')).encode(),
                          PLAYER_SOCKET)
            print('Force seek: N{} v{} -> {:.4f}%'.format(
                note, velocity, position * 100), flush=True)
        except OSError as exc:
            print('Force seek unavailable: {}'.format(exc), flush=True)

    def send_clip_cue(velocity, position=None):
        if velocity < CLIP_CUE_MIN or velocity > CLIP_CUE_MAX:
            print('Force clip cue ignored: velocity {} is outside {}-{}'.format(
                velocity, CLIP_CUE_MIN, CLIP_CUE_MAX), flush=True)
            return
        if velocity in RESERVED_CLIP_CUES:
            print('Force clip cue ignored: velocity {} is reserved'.format(
                velocity), flush=True)
            return
        try:
            clip_count = len([
                name for name in os.listdir(CLIPS_DIR)
                if name.lower().endswith('.mp4')])
            cue_number = velocity
            # Some Force velocity lanes skip the exact value immediately after
            # the current range. Treat that one-above-final value as an alias
            # for the final clip instead of letting player modulo wrap to ZEDD.
            if cue_number == clip_count + 1:
                cue_number = clip_count
            elif cue_number > clip_count:
                print('Force clip cue ignored: clip {} does not exist ({} clips)'.format(
                    velocity, clip_count), flush=True)
                return
            index = cue_number - 1
            payload = dict(action='play_index', index=index, **event_fields())
            if position is not None:
                payload['position'] = position
            player.sendto(json.dumps(payload, separators=(',', ':')).encode(),
                          PLAYER_SOCKET)
            print('Force clip cue: G8 v{} -> clip {}'.format(
                velocity, cue_number), flush=True)
            if position is not None:
                print('Force combined cue+seek: clip {} at {:.4f}%'.format(
                    cue_number, position * 100), flush=True)
        except OSError as exc:
            print('Force clip cue unavailable: {}'.format(exc), flush=True)

    def send_audio_patch_cue(channel, velocity):
        if velocity < AUDIO_PATCH_MIN or velocity > AUDIO_PATCH_MAX:
            print('Force audio patch cue ignored: velocity {} is outside {}-{}'.format(
                velocity, AUDIO_PATCH_MIN, AUDIO_PATCH_MAX), flush=True)
            return
        send(channel, AUDIO_PATCH_CC, velocity)
        print('Force audio patch cue: C#8 v{} -> AUDIO{:02d}'.format(
            velocity, velocity), flush=True)

    def send_shader_event(action, **fields):
        try:
            payload = dict(action=action, timestamp_ns=time.monotonic_ns(), **fields)
            player.sendto(json.dumps(payload, separators=(',', ':')).encode(),
                          SHADER_BEAT_SOCKET)
        except OSError:
            pass

    def send_stutter(control, value, raw, player_changed):
        if player_changed:
            try:
                payload = ('{{"action":"stutter_control","control":"{}",'
                           '"value":{}}}').format(control, value)
                player.sendto(payload.encode(), PLAYER_SOCKET)
            except OSError:
                # The socket is briefly absent while video.service starts.
                pass
        send_shader_event('stutter_control', control=control, raw=raw)

    while True:
        try:
            name = find_force_port()
            if not name:
                print('Force MIDI port unavailable; retrying', flush=True)
                time.sleep(2)
                continue
            print('Forwarding Force CC from {} to {}:{}'.format(
                name, *DESTINATION), flush=True)
            with mido.open_input(name) as port:
                beat_counter = ForceBeatCounter()
                print('Force beat LEDs waiting for MIDI Start or Song Position',
                      flush=True)
                current = {}
                target = {}
                curved_pixel_cues = set()
                last_sent = {}
                last_stutter_sent = {}
                last_stutter_frame = {}
                previous = time.monotonic()
                last_reassert = previous
                last_local_reassert = previous
                while True:
                    pending_midi = list(port.iter_pending())
                    skip_note = set()
                    for note_index, message in enumerate(pending_midi):
                        if note_index in skip_note:
                            continue
                        if (message.type == 'note_on' and message.velocity > 0
                                and note_index + 1 < len(pending_midi)):
                            following = pending_midi[note_index + 1]
                            if following.type == 'note_on' and following.velocity > 0:
                                if message.note == CLIP_CUE_NOTE:
                                    position = note_to_position(following.note,
                                                                following.velocity)
                                    if position is not None:
                                        send_clip_cue(message.velocity, position)
                                        skip_note.add(note_index + 1)
                                        continue
                                elif following.note == CLIP_CUE_NOTE:
                                    position = note_to_position(message.note,
                                                                message.velocity)
                                    if position is not None:
                                        send_clip_cue(following.velocity, position)
                                        skip_note.add(note_index + 1)
                                        continue
                        if message.type == 'clock':
                            send_shader_event('clock')
                            beat_event = beat_counter.update('clock')
                            if beat_event:
                                udp.sendto(beat_event.encode(), BEAT_DESTINATION)
                            continue
                        if message.type == 'songpos':
                            beat_counter.update('songpos', message.pos)
                            print('Force beat song position: {}'.format(message.pos),
                                  flush=True)
                            continue
                        if message.type in ('start', 'continue', 'stop'):
                            send_shader_event(message.type)
                            beat_event = beat_counter.update(message.type)
                            print('Force beat transport: {}'.format(message.type),
                                  flush=True)
                            if beat_event:
                                udp.sendto(beat_event.encode(), BEAT_DESTINATION)
                            continue
                        if message.type == 'note_on' and message.velocity > 0:
                            if message.note == AUDIO_PATCH_CUE_NOTE:
                                send_audio_patch_cue(message.channel, message.velocity)
                            elif message.note == VIDEO_PATCH_CUE_NOTE:
                                print('Force video patch cue reserved: D8 v{}'.format(
                                    message.velocity), flush=True)
                            elif message.note == INPUT_TRIM_CUE_NOTE:
                                key = (message.channel, INPUT_TRIM_CC)
                                if key not in current:
                                    current[key] = float(last_sent.get(key, 63))
                                target[key] = message.velocity
                                print('Force input trim cue: D#8 v{} -> CC98 {}'.format(
                                    message.velocity, message.velocity), flush=True)
                            elif message.note == CLIP_CUE_NOTE:
                                send_clip_cue(message.velocity)
                            elif message.note == PIXEL_CUE_NOTE:
                                key = (message.channel, PIXEL_CC)
                                if key not in current:
                                    current[key] = float(last_sent.get(key, 63))
                                target[key] = message.velocity
                                curved_pixel_cues.add(key)
                                print('Force pixel cue: F#8 v{} -> CC97 {}'.format(
                                    message.velocity, message.velocity), flush=True)
                            else:
                                send_seek(message.note, message.velocity)
                            continue
                        if message.type != 'control_change':
                            continue
                        key = (message.channel, message.control)
                        if message.control == INPUT_TRIM_CC:
                            print('Force input trim CC98: {}'.format(
                                message.value), flush=True)
                        curved_pixel_cues.discard(key)
                        if message.control == STUTTER_CC:
                            stutter_value = scale_stutter(message.value)
                            if last_stutter_sent.get(key) != message.value:
                                send_stutter('normal', stutter_value,
                                             message.value,
                                             last_stutter_frame.get(key) != stutter_value)
                                last_stutter_sent[key] = message.value
                                last_stutter_frame[key] = stutter_value
                        elif message.control == STUTTER_REVERSE_CC:
                            stutter_value = scale_stutter_reverse(message.value)
                            if last_stutter_sent.get(key) != message.value:
                                send_stutter('reverse', stutter_value,
                                             message.value,
                                             last_stutter_frame.get(key) != stutter_value)
                                last_stutter_sent[key] = message.value
                                last_stutter_frame[key] = stutter_value
                        if message.control not in SMOOTH_CC:
                            send(*key, message.value)
                            continue
                        if key not in current:
                            if last_sent.get(key) == message.value:
                                # Reassert held Q-Link values after renderer restarts.
                                send(*key, message.value)
                            current[key] = float(last_sent.get(key, 63))
                        target[key] = message.value

                    now = time.monotonic()
                    blend = 1.0 - math.exp(-min(now - previous, 0.1) / SMOOTH_SECONDS)
                    previous = now
                    for key in list(target):
                        key_blend = (pixel_cue_lerp(current[key], target[key])
                                     if key in curved_pixel_cues else blend)
                        current[key] += (target[key] - current[key]) * key_blend
                        if abs(target[key] - current[key]) < 0.5:
                            current[key] = float(target[key])
                        value = round(current[key])
                        if value != last_sent.get(key):
                            send(*key, value)
                            last_sent[key] = value
                        if current[key] == target[key]:
                            del current[key]
                            del target[key]
                            curved_pixel_cues.discard(key)
                    if now - last_reassert >= REASSERT_SECONDS:
                        for key, value in last_sent.items():
                            if key[1] not in MIXER_CC:
                                send(*key, value)
                        last_reassert = now
                    if now - last_local_reassert >= 2.0:
                        for key, value in last_sent.items():
                            if key[1] in MIXER_CC:
                                send(*key, value)
                        last_local_reassert = now
                    time.sleep(0.002)
        except Exception as exc:
            print('Force MIDI connection lost: {}'.format(exc), flush=True)
            time.sleep(2)


if __name__ == '__main__':
    main()
