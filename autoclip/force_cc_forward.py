#!/usr/bin/env python3
"""Forward Akai Force USB gadget CCs to autowaaave's separate visual layer."""

import os
import socket
import time
import math

import mido


MIDI_PORT_MATCH = os.environ.get('AUTOEYEZ_FORCE_MIDI_PORT', 'f_midi:')
DESTINATION = (os.environ.get('AUTOEYEZ_FORCE_DEST_HOST', '10.0.0.1'), 5010)
PLAYER_SOCKET = os.environ.get('AUTOEYEZ_PLAYER_SOCKET', '/tmp/player_seek.sock')
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
PIXEL_CUE_NOTE = 126  # F#8 on Force.
PIXEL_CC = 97
PIXEL_CUE_LERP_MIN = 0.05
PIXEL_CUE_LERP_MAX = 0.5
STUTTER_CC = 22
STUTTER_REVERSE_CC = 24
SMOOTH_CC = {16, 17, 18, 19, 20, 21, 23, 94, 95, 96, 97, 98, 122}
SMOOTH_SECONDS = 0.045  # About 90% of a sudden change in 0.10 seconds.
REASSERT_SECONDS = 0.25  # Repair a missed UDP update without waiting for a Q-Link move.
STUTTER_MAX_FRAMES = 30
STUTTER_MIN_FRAMES = 2


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

    def send(channel, control, value):
        udp.sendto(bytes((0xb0 | channel, control, value)), DESTINATION)

    def send_seek(note, velocity):
        position = note_to_position(note, velocity)
        if position is None:
            return
        try:
            player.sendto(('{{"position":{}}}'.format(position)).encode(),
                          PLAYER_SOCKET)
            print('Force seek: N{} v{} -> {:.4f}%'.format(
                note, velocity, position * 100), flush=True)
        except OSError as exc:
            print('Force seek unavailable: {}'.format(exc), flush=True)

    def send_clip_cue(velocity):
        if velocity < CLIP_CUE_MIN or velocity > CLIP_CUE_MAX:
            print('Force clip cue ignored: velocity {} is outside {}-{}'.format(
                velocity, CLIP_CUE_MIN, CLIP_CUE_MAX), flush=True)
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
            payload = '{{"action":"play_index","index":{}}}'.format(index)
            player.sendto(payload.encode(), PLAYER_SOCKET)
            print('Force clip cue: G8 v{} -> clip {}'.format(
                velocity, cue_number), flush=True)
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

    def send_stutter(control, value):
        try:
            payload = ('{{"action":"stutter_control","control":"{}",'
                       '"value":{}}}').format(control, value)
            player.sendto(payload.encode(), PLAYER_SOCKET)
        except OSError:
            # The socket is briefly absent while video.service starts.
            pass

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
                current = {}
                target = {}
                curved_pixel_cues = set()
                last_sent = {}
                last_stutter_sent = {}
                previous = time.monotonic()
                last_reassert = previous
                while True:
                    for message in port.iter_pending():
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
                            if last_stutter_sent.get(key) != stutter_value:
                                send_stutter('normal', stutter_value)
                                last_stutter_sent[key] = stutter_value
                        elif message.control == STUTTER_REVERSE_CC:
                            stutter_value = scale_stutter_reverse(message.value)
                            if last_stutter_sent.get(key) != stutter_value:
                                send_stutter('reverse', stutter_value)
                                last_stutter_sent[key] = stutter_value
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
                            send(*key, value)
                        last_reassert = now
                    time.sleep(0.01)
        except Exception as exc:
            print('Force MIDI connection lost: {}'.format(exc), flush=True)
            time.sleep(2)


if __name__ == '__main__':
    main()
