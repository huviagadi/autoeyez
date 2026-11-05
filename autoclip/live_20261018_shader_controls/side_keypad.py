from patch_transport import FrameInbox
import copy,math
#!/usr/bin/env python3
"""SIDE-KEYBOARD clip cue controller for Autoclip."""

import glob
import json
import os
import select
import socket
import threading
import time

from evdev import InputDevice, categorize, ecodes
from command_ipc import CommandSender, MIXER_COMMAND_SOCKET
from shader_controls import (CONTROL_LABELS, ShaderControls, default_patch,
                             validate_patch)


VID = '0816'
PID = '2475'
SERIAL = '253E01678182'
PLAYER_SOCKET = '/tmp/player_seek.sock'
CONTROL_SOCKET = '/tmp/keypad_cue.sock'
PLAYER_STATE = '/tmp/player_state.json'
STATE_FILE = '/home/admin/keypad_cues.json'
SHADER_STATE_FILE = '/home/admin/shader_audio_controls.json'
HOLD_SECONDS = 0.50

# Physical button order from the keypad's WebHub key table.
BUTTON_CODES = {
    ecodes.KEY_KP7: 0, ecodes.KEY_KP8: 1, ecodes.KEY_KP9: 2,
    ecodes.KEY_KPMINUS: 3,
    ecodes.KEY_KP4: 4, ecodes.KEY_KP5: 5, ecodes.KEY_KP6: 6,
    ecodes.KEY_KPPLUS: 7,
    ecodes.KEY_KP1: 8, ecodes.KEY_KP2: 9, ecodes.KEY_KP3: 10,
    ecodes.KEY_KPSLASH: 11, ecodes.KEY_KPASTERISK: 12,
    ecodes.KEY_KP0: 13, ecodes.KEY_KPDOT: 14,
    ecodes.KEY_KPENTER: 15,
}

# Knob rotations. Knob 3 is assigned F17/F18 at service startup because the
# factory map duplicates knob 2's volume events.
KNOB_CODES = {
    ecodes.KEY_BACKSPACE: (0, 1), ecodes.KEY_SPACE: (0, -1),
    ecodes.KEY_VOLUMEDOWN: (1, -1), ecodes.KEY_VOLUMEUP: (1, 1),
    ecodes.KEY_F17: (2, 1), ecodes.KEY_F18: (2, -1),
}
KNOB_PRESS_CODES = {ecodes.KEY_F19: 0, ecodes.KEY_F20: 1,
                    ecodes.KEY_F21: 2}


def default_cues():
    return [(i + 1) / 16.0 for i in range(16)]


def atomic_json(path, value):
    tmp = path + '.tmp'
    with open(tmp, 'w') as f:
        json.dump(value, f, separators=(',', ':'))
    os.replace(tmp, path)


class WebHubLighting:
    """Minimal safe subset of the keypad's 64-byte WebHub protocol."""

    def __init__(self):
        self.fd = None
        self.path = None
        self.restore_frame = None
        self.lock = threading.Lock()
        self.generation = 0
        self.next_connect = 0.0
        self.send(self._off())

    def _find_hidraw(self):
        for path in glob.glob('/sys/class/hidraw/hidraw*'):
            device = os.path.realpath(os.path.join(path, 'device'))
            try:
                with open(os.path.join(device, 'uevent')) as f:
                    uevent = f.read().lower()
            except OSError:
                continue
            # Interface 1.2 is the vendor-defined, bidirectional WebHub link.
            if ('00000816:00002475' in uevent and
                    os.path.basename(os.path.dirname(device)).endswith(':1.2')):
                return '/dev/' + os.path.basename(path)
        # The interface test above can vary by kernel sysfs layout. Find the
        # vendor interface by its 64-byte report descriptor as a fallback.
        for path in glob.glob('/sys/class/hidraw/hidraw*'):
            device = os.path.realpath(os.path.join(path, 'device'))
            if '0816:2475' in device.lower() and device.endswith('.0006'):
                return '/dev/' + os.path.basename(path)
        return None

    def _connect(self):
        # Connection/configuration owns the fd; it never calls public send().
        with self.lock:
            if self.fd is not None:
                return True
            if time.monotonic() < self.next_connect:
                return False
            self.next_connect = time.monotonic() + 2.0
            try:
                path = self._find_hidraw()
                if not path:
                    return False
                self.fd = os.open(path, os.O_RDWR | os.O_NONBLOCK)
                self.path = path
                response = self._query_locked(bytes([0x06, 0x0A]))
                if response and len(response) >= 16 and response[0:2] == b'\xaa\x0a':
                    frame = bytearray(64)
                    frame[0:2] = b'\x06\x0b'
                    frame[2:16] = response[2:16]
                    self.restore_frame = bytes(frame)
                self._ensure_knob3_unique_locked()
                return True
            except OSError:
                self._close_locked()
                return False

    def _close_locked(self):
        if self.fd is not None:
            try:
                os.close(self.fd)
            except OSError:
                pass
        self.fd = None
        self.path = None

    def close(self):
        with self.lock:
            self._close_locked()
            self.next_connect = time.monotonic() + 2.0

    def _write_locked(self, frame):
        if os.write(self.fd, frame) != len(frame):
            raise OSError('short keypad report write')

    def _query_locked(self, prefix, timeout=0.5):
        if self.fd is None:
            return None
        frame = bytearray(64)
        frame[:len(prefix)] = prefix
        self._write_locked(frame)
        readable, _, _ = select.select([self.fd], [], [], timeout)
        return os.read(self.fd, 64) if readable else None

    def query(self, prefix, timeout=0.5):
        with self.lock:
            try:
                return self._query_locked(prefix, timeout)
            except OSError:
                self._close_locked()
                self.next_connect = time.monotonic() + 2.0
                return None

    def send(self, frame):
        if not self._connect():
            return
        with self.lock:
            if self.fd is None:
                return
            try:
                self._write_locked(frame)
            except OSError:
                self._close_locked()
                self.next_connect = time.monotonic() + 2.0

    def _write_key_locked(self, index, usage):
        frame = bytearray(64)
        offset = index * 4
        frame[:12] = bytes([
            0x06, 0x10, 0x07, offset & 0xff, offset >> 8,
            0x00, 0x00, 0x00, 0x20, 0x00, usage, 0x00,
        ])
        self._write_locked(frame)
        time.sleep(0.03)

    def _ensure_knob3_unique_locked(self):
        response = self._query_locked(bytes([0x06, 0x08, 0x3A, 56, 0, 0, 0]))
        if not response or len(response) < 52:
            return
        left = response[8 + 36:8 + 40]
        right = response[8 + 40:8 + 44]
        if left != bytes([0x20, 0x00, 0x6C, 0x00]):
            self._write_key_locked(23, 0x6C)
        if right != bytes([0x20, 0x00, 0x6D, 0x00]):
            self._write_key_locked(24, 0x6D)
        for index, usage in ((16, 0x6E), (19, 0x6F), (22, 0x70)):
            offset = (index - 14) * 4
            if response[8 + offset:12 + offset] != bytes((0x20, 0, usage, 0)):
                self._write_key_locked(index, usage)

    def cancel(self):
        # Stop blink/fade workers without starting a new connection attempt.
        self.generation += 1
        with self.lock:
            if self.fd is not None:
                try:
                    self._write_locked(self._off())
                except OSError:
                    self._close_locked()

    @staticmethod
    def _orange(mode=1, value=255):
        frame = bytearray(64)
        # Fixed orange HSV. V is stepped in software for a reliable fade.
        frame[:16] = bytes([
            0x06, 0x0B, 0x0B, 0x00, 0x00, 0x01, 0x00, mode,
            0x04, 0x01, 0x00, 0x01, 0x00, 21, 255, value,
        ])
        return bytes(frame)

    @staticmethod
    def _off():
        frame = bytearray(64)
        # Keep the pad dark between cue animations.
        frame[:16] = bytes([
            0x06, 0x0B, 0x0B, 0x00, 0x00, 0x01, 0x00, 0x00,
            0x04, 0x01, 0x00, 0x01, 0x00, 21, 255, 255,
        ])
        return bytes(frame)

    def press(self):
        self.generation += 1
        self.send(self._orange(1))
        return self.generation

    def held(self, generation):
        def blink():
            # The keypad's built-in blink mode only pulses once on this unit,
            # so hold feedback is driven explicitly until release.
            while generation == self.generation:
                self.send(self._orange(value=255))
                time.sleep(0.18)
                if generation != self.generation:
                    break
                self.send(self._off())
                time.sleep(0.18)
        threading.Thread(target=blink, daemon=True).start()

    def release(self, generation, fade=False):
        if generation != self.generation:
            return
        # Invalidate the hold blink (and any older fade) immediately.
        self.generation += 1
        fade_generation = self.generation

        def finish():
            if fade:
                for value in (255, 210, 165, 120, 80, 45, 20, 8):
                    if fade_generation != self.generation:
                        return
                    self.send(self._orange(value=value))
                    time.sleep(0.035)
            if fade_generation == self.generation:
                self.send(self._off())
        threading.Thread(target=finish, daemon=True).start()


class CueController:
    def __init__(self):
        self.data = {'version': 1, 'current': default_cues(), 'patches': {}}
        self._load_state()
        self.shader_data = {'version': 1, 'current': default_patch(), 'patches': {}}
        self._load_shader_state()
        self.shader = ShaderControls(self.shader_data['current'])
        self.mixer = CommandSender(MIXER_COMMAND_SOCKET, 'mixer')
        self.feedback_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.held_slot = None
        self.press_time = 0.0
        self.was_paused = False
        self.hold_active = False
        self.hold_generation = 0
        self.state_lock = threading.Lock()
        self.lighting = WebHubLighting()
        self.player_sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.control_sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        if os.path.exists(CONTROL_SOCKET):
            os.remove(CONTROL_SOCKET)
        self.control_sock.bind(CONTROL_SOCKET)
        os.chmod(CONTROL_SOCKET, 0o666)
        self.devices = []
        self.patch_inbox=FrameInbox('cues',self._prepare_patch,self._apply_patch)
        self._publish_shader()

    def _load_state(self):
        try:
            with open(STATE_FILE) as f:
                saved = json.load(f)
            current = saved.get('current', [])
            if len(current) == 16:
                self.data = saved
        except (OSError, ValueError, TypeError):
            pass

    def _save_state(self):
        atomic_json(STATE_FILE, self.data)

    def _load_shader_state(self):
        try:
            with open(SHADER_STATE_FILE) as f:
                saved = json.load(f)
            if saved.get('version') != 1 or not isinstance(saved.get('patches'), dict):
                raise ValueError('shader state')
            saved['current'] = validate_patch(saved['current'])
            saved['patches'] = {str(int(key)): validate_patch(value)
                                for key, value in saved['patches'].items()
                                if 1 <= int(key) <= 32}
            self.shader_data = saved
        except (OSError, ValueError, TypeError, KeyError):
            pass

    def _save_shader_state(self):
        self.shader_data['current'] = self.shader.snapshot()
        atomic_json(SHADER_STATE_FILE, self.shader_data)

    def _publish_shader(self):
        self.mixer.send({'action': 'shader_patch', 'value': self.shader.snapshot()})

    def _shader_feedback(self, slot=-1):
        role, band = self.shader.role, self.shader.band
        values = (self.shader.patch['offsets'][role] if band < 0 else
                  self.shader.patch['reactivity'][role][band])
        try:
            with open('/tmp/mixer_state.json') as f:
                mixer = json.load(f)
            source = (mixer.get('ch_a', 0), mixer.get('luma_src', 0) - 1,
                      mixer.get('ch_b', 0))[role]
        except (OSError, ValueError):
            source = 0
        value = values[slot] if slot in (0, 1, 2) else 0
        base = self.shader.patch['offsets'][role][slot] if slot in (0, 1, 2) else 64
        preview = (max(0, min(127, base + value - 64))
                   if band >= 0 and slot in (0, 1, 2) else base)
        labels = CONTROL_LABELS.get(source, ('PARAM 1', 'PARAM 2', 'PARAM 3'))
        label = labels[slot] if slot in (0, 1, 2) else 'SOURCE'
        line = 'SHADER_UI:{},{},{},{},{},{},{},{}'.format(
            role, band, source, slot, value, base, preview, label)
        try:
            self.feedback_sock.sendto(line.encode(), ('10.0.0.1', 5005))
        except OSError as error:
            print('Shader overlay send failed: {}'.format(error), flush=True)

    def _player_state(self):
        try:
            with open(PLAYER_STATE) as f:
                return json.load(f)
        except (OSError, ValueError):
            return {}

    def _send_player(self, message):
        try:
            self.player_sock.sendto(json.dumps(message).encode(), PLAYER_SOCKET)
        except OSError as error:
            print('Player command failed: {}'.format(error), flush=True)

    def _preview(self):
        if self.held_slot is None:
            return
        position = self.data['current'][self.held_slot]
        self._send_player({'action': 'cue_preview', 'position': position})
        print('Cue {:02d}: {:.4f}'.format(self.held_slot + 1, position), flush=True)

    def button_down(self, slot):
        with self.state_lock:
            if self.held_slot is not None:
                return
            state = self._player_state()
            self.was_paused = bool(state.get('paused', False))
            self.held_slot = slot
            self.hold_active = False
            self.press_time = time.monotonic()
            self.hold_generation = self.lighting.press()
            generation = self.hold_generation
            # Freeze the cue frame immediately. If this becomes a hold, keep
            # the same decoded frame through the threshold with no jump.
            self._preview()

        def held_feedback():
            time.sleep(HOLD_SECONDS)
            with self.state_lock:
                if (self.held_slot != slot or
                        generation != self.hold_generation):
                    return
                self.hold_active = True
                self.lighting.held(generation)
        threading.Thread(target=held_feedback, daemon=True).start()

    def button_up(self, slot):
        with self.state_lock:
            if self.held_slot != slot:
                return
            was_hold = self.hold_active
            if not self.was_paused:
                self._send_player({'action': 'play'})
            self.held_slot = None
            self.hold_active = False
            generation = self.hold_generation
        self.lighting.release(
            generation,
            fade=not was_hold)

    def turn_knob(self, knob, direction):
        with self.state_lock:
            if self.held_slot is None:
                self.shader.turn(knob, 2 * direction)
                self._publish_shader()
                self._save_shader_state()
                self._shader_feedback(knob)
                return
            if not self.hold_active:
                return
            state = self._player_state()
            duration = max(float(state.get('duration', 0.0)), 0.001)
            frame_period = float(state.get('frame_period', 1.0 / 30.0))
            seconds = (1.0, 0.1, frame_period)[knob]
            delta = direction * seconds / duration
            maximum = max(0.0, 1.0 - frame_period / duration)
            self.data['current'][self.held_slot] = min(
                maximum, max(0.0, self.data['current'][self.held_slot] + delta))
            self._save_state()
            self._preview()

    def press_knob(self, role):
        with self.state_lock:
            if self.held_slot is not None:
                return
            self.shader.select_role(role)
            self._shader_feedback()

    def _validate_cue_state(self,state):
        if not isinstance(state,dict) or type(state.get('version')) is not int or state['version']!=1 or set(state)-{'version','current','patches','patch_identities'}:raise ValueError('cue state schema')
        patches=state.get('patches');identities=state.get('patch_identities',{})
        if not isinstance(patches,dict) or len(patches)>32 or not isinstance(identities,dict) or len(identities)>32:raise ValueError('cue patches schema')
        groups=[state.get('current')]
        for slot,cues in patches.items():
            if not isinstance(slot,str) or not slot.isdigit() or not 1<=int(slot)<=32 or str(int(slot))!=slot:raise ValueError('cue slot schema')
            groups.append(cues)
        if any(not isinstance(cues,list) or len(cues)!=16 or any(type(v) not in (int,float) or not math.isfinite(v) or not 0<=v<=1 for v in cues) for cues in groups):raise ValueError('cue values schema')
        if any(slot not in patches or not isinstance(identity,str) or len(identity)!=64 or any(c not in '0123456789abcdef' for c in identity) for slot,identity in identities.items()):raise ValueError('cue patch identities')

    def _prepare_patch(self,payload):
        if not isinstance(payload,dict) or set(payload)!= {'slot','operation','patch_identity','require_identity'} or type(payload['slot']) is not int or not 0<=payload['slot']<32 or payload['operation'] not in ('save','load') or type(payload['require_identity']) is not bool or not isinstance(payload['patch_identity'],str) or len(payload['patch_identity'])!=64 or any(c not in '0123456789abcdef' for c in payload['patch_identity']):raise ValueError('cue patch request')
        with self.state_lock:before=copy.deepcopy(self.data)
        self._validate_cue_state(before);candidate=copy.deepcopy(before);slot=str(payload['slot']+1)
        if payload['operation']=='load':
            if payload['require_identity'] and candidate.get('patch_identities',{}).get(slot)!=payload['patch_identity']:raise ValueError('video/cue save identity mismatch')
            candidate['current']=list(candidate['patches'].get(slot,default_cues()))
            rollback={'restore_current':before['current']}
        else:
            candidate['patches'][slot]=list(candidate['current']);candidate.setdefault('patch_identities',{})[slot]=payload['patch_identity']
            rollback={'restore_slot':slot,'previous_patch':before['patches'].get(slot),'previous_identity':before.get('patch_identities',{}).get(slot)}
        self._validate_cue_state(candidate);return candidate,rollback

    def _apply_patch(self,payload):
        if not isinstance(payload,dict):raise ValueError('cue state schema')
        if 'restore_slot' in payload or 'restore_current' in payload:
            with self.state_lock:candidate=copy.deepcopy(self.data)
            if 'restore_slot' in payload:
                slot=payload['restore_slot'];previous=payload['previous_patch'];identity=payload.get('previous_identity')
                if not isinstance(slot,str) or not slot.isdigit() or not 1<=int(slot)<=32 or str(int(slot))!=slot:raise ValueError('cue rollback slot')
                if previous is None:candidate['patches'].pop(slot,None)
                else:candidate['patches'][slot]=previous
                if identity is None:candidate.setdefault('patch_identities',{}).pop(slot,None)
                else:candidate.setdefault('patch_identities',{})[slot]=identity
            else:candidate['current']=payload['restore_current']
            payload=candidate
        self._validate_cue_state(payload)
        from patch_transport import atomic_json as durable_json
        durable_json(STATE_FILE,payload,durable=True)
        with self.state_lock:self.data=copy.deepcopy(payload)

    def handle_control(self, message):
        action = message.get('action')
        slot = str(int(message.get('slot', 0)) + 1)
        if action == 'save_patch':
            self.data.setdefault('patches', {})[slot] = list(self.data['current'])
            self._save_state()
            print('Saved cues with video patch {}'.format(slot), flush=True)
        elif action == 'load_patch':
            cues = self.data.setdefault('patches', {}).get(slot, default_cues())
            self.data['current'] = list(cues)
            self._save_state()
            print('Loaded cues for video patch {}'.format(slot), flush=True)
        elif action == 'shader_band':
            with self.state_lock:
                self.shader.select_band(int(message['band']))
                self._shader_feedback()
        elif action == 'shader_audio_save':
            with self.state_lock:
                self.shader_data['patches'][slot] = self.shader.snapshot()
                self._save_shader_state()
        elif action == 'shader_audio_load':
            with self.state_lock:
                self.shader.load(self.shader_data['patches'].get(slot, default_patch()))
                self._publish_shader()
                self._save_shader_state()
                self._shader_feedback()

    def _close_inputs(self):
        for device in self.devices:
            try:
                device.close()
            except OSError:
                pass
        self.devices = []

    def _disconnect_inputs(self):
        with self.state_lock:
            resume = self.held_slot is not None and not self.was_paused
            self.held_slot = None
            self.hold_active = False
            self.hold_generation = 0
            self.lighting.cancel()
            if resume:
                self._send_player({'action': 'play'})
        self._close_inputs()
        self.lighting.close()

    def _open_inputs(self):
        paths = glob.glob('/dev/input/by-id/usb-SDINNOVATION_SIDE-KEYBOARD_*event*')
        opened = []
        seen = set()
        try:
            for path in paths:
                real = os.path.realpath(path)
                if real in seen:
                    continue
                seen.add(real)
                device = InputDevice(path)
                opened.append(device)
                device.grab()
                print('Grabbed {} ({})'.format(device.name, device.path), flush=True)
        except OSError:
            for device in opened:
                try:
                    device.close()
                except OSError:
                    pass
            raise
        self.devices = opened

    def _poll_inputs(self):
        readers = {device.fd: device for device in self.devices}
        readers[self.control_sock.fileno()] = None
        readable, _, _ = select.select(list(readers), [], [], 0.02)
        for fd in readable:
            device = readers[fd]
            if device is None:
                try:
                    data = self.control_sock.recv(1024)
                    self.handle_control(json.loads(data.decode()))
                except (ValueError, UnicodeError) as error:
                    print('Invalid keypad control: {}'.format(error), flush=True)
                continue
            try:
                events = list(device.read())
            except BlockingIOError:
                continue
            for event in events:
                if event.type != ecodes.EV_KEY or event.value == 2:
                    continue
                if event.code in BUTTON_CODES:
                    if event.value == 1:
                        self.button_down(BUTTON_CODES[event.code])
                    else:
                        self.button_up(BUTTON_CODES[event.code])
                elif event.code in KNOB_CODES and event.value == 1:
                    knob, direction = KNOB_CODES[event.code]
                    self.turn_knob(knob, direction)
                elif event.code in KNOB_PRESS_CODES and event.value == 1:
                    self.press_knob(KNOB_PRESS_CODES[event.code])
                elif event.value == 1:
                    print('Ignored key code {} ({}) from {}'.format(
                        event.code, ecodes.KEY.get(event.code, 'unknown'),
                        device.path), flush=True)

    def run(self):
        self._save_state()
        input_retry = 0.0
        while True:
            self.patch_inbox.poll()
            if not self.devices and time.monotonic() >= input_retry:
                input_retry = time.monotonic() + 2.0
                try:
                    self._open_inputs()
                except OSError as error:
                    print('Keypad input unavailable: {}'.format(error), flush=True)
            try:
                # The control socket and patch owner remain responsive unplugged.
                self._poll_inputs()
            except OSError as error:
                print('Keypad reconnect: {}'.format(error), flush=True)
                self._disconnect_inputs()
                input_retry = time.monotonic() + 2.0
                time.sleep(.02)


if __name__ == '__main__':
    CueController().run()
