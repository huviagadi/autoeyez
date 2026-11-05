"""Bounded, acknowledged local control delivery. No audio/video data use this path.

ACK means queued in the live receiver, not durable across a receiver process crash.
Action commands stay ordered; only explicitly safe continuous values are merged.
"""
import collections
import grp
import json
import logging
import os
import select
import socket
import threading
import uuid
from source_catalog import MAX_SOURCE_INDEX, MAX_LUMA_SOURCE_INDEX

PLAYER_COMMAND_SOCKET = '/tmp/autoeyez_player_commands.sock'
MIXER_COMMAND_SOCKET = '/tmp/autoeyez_mixer_commands.sock'
MAX_PACKET = 4096
CAPACITY = 256
CONTINUOUS = frozenset(('mix', 'luma', 'luma_high', 'luma_feather', 'mosh',
                        'shader_patch', 'shader_fft', 'force_offset'))
LOG = logging.getLogger('command_ipc')


def coalesce_index(pending, command):
    action = command.get('action')
    if action not in CONTINUOUS:
        return None
    for index in range(len(pending) - 1, -1, -1):
        previous = pending[index]['action']
        # Threshold setters constrain each other: never merge across the other.
        if previous not in CONTINUOUS or {action, previous} == {'luma', 'luma_high'}:
            break
        if previous == action and (action != 'force_offset' or
                                   pending[index].get('cc') == command.get('cc')):
            return index
    return None


def validate_command(command, target):
    if not isinstance(command, dict):
        raise ValueError('command must be an object')
    action = command.get('action')
    if target == 'player':
        if action not in ('next', 'prev', 'play', 'pause', 'loop_on', 'loop_off',
                          'play_index', 'play_loop'):
            raise ValueError('unknown player action')
        if action in ('play_index', 'play_loop'):
            if type(command.get('index')) is not int or command['index'] < 0:
                raise ValueError('invalid clip index')
        if action == 'play_loop' and type(command.get('loop')) is not bool:
            raise ValueError('invalid loop state')
    elif target == 'mixer':
        if action == 'force_offset':
            from force_offset_map import MIXER_CC
            cc, value = command.get('cc'), command.get('value')
            if type(cc) is not int or cc not in MIXER_CC or type(value) is not int or not 0 <= value <= 127:
                raise ValueError('invalid Force offset')
            return {'action': action, 'cc': cc, 'value': value}
        if action == 'shader_patch':
            from shader_controls import validate_patch
            return {'action': action, 'value': validate_patch(command.get('value'))}
        if action == 'shader_fft':
            values = command.get('value')
            if not isinstance(values, list) or len(values) != 3 or any(
                    type(value) not in (int, float) or not 0 <= value <= 8
                    for value in values):
                raise ValueError('invalid shader FFT bands')
            return {'action': action, 'value': list(map(float, values))}
        ranges = {'ch_a': (0, MAX_SOURCE_INDEX), 'ch_b': (0, MAX_SOURCE_INDEX), 'mix': (0, 127),
                  'luma': (0, 127), 'luma_high': (0, 127), 'luma_src': (0, MAX_LUMA_SOURCE_INDEX),
                  'luma_feather': (0, 4), 'mosh': (0, 127)}
        if action == 'luma_high_en':
            if type(command.get('value')) is not bool:
                raise ValueError('invalid high-key enabled state')
        elif action in ranges:
            value = command.get('value')
            low, high = ranges[action]
            if type(value) is not int or not low <= value <= high:
                raise ValueError('invalid mixer value')
        else:
            raise ValueError('unknown mixer action')
    else:
        raise ValueError('unknown target')
    # Keep only the fields consumed by the existing handlers.
    keys = ('action', 'index', 'loop') if target == 'player' else ('action', 'value')
    return {key: command[key] for key in keys if key in command}


class CommandQueue:
    def __init__(self, capacity=CAPACITY):
        self.capacity = capacity
        self.pending = []
        self.lock = threading.Lock()
        self.coalesced = 0

    def put(self, command):
        with self.lock:
            index = coalesce_index(self.pending, command)
            if index is not None:
                del self.pending[index]
                self.coalesced += 1
            elif len(self.pending) >= self.capacity:
                raise BufferError('command queue full')
            self.pending.append(dict(command))

    def drain(self, limit=CAPACITY):
        with self.lock:
            result = self.pending[:limit]
            del self.pending[:limit]
            return result


class CommandReceiver:
    """Socket thread accepts controls; the owning frame loop applies them."""
    def __init__(self, path, target, capacity=CAPACITY):
        self.path, self.target = path, target
        self.queue = CommandQueue(capacity)
        self.accepted = 0
        self.replays = 0
        self.last_sequence = collections.OrderedDict()
        self.stop = threading.Event()
        # Never steal a socket from a live receiver.
        if os.path.exists(path):
            probe = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                probe.settimeout(0.2)
                probe.connect(path)
            except (ConnectionRefusedError, FileNotFoundError):
                try:
                    os.unlink(path)
                except FileNotFoundError:
                    pass
            else:
                raise RuntimeError('command receiver already running: ' + path)
            finally:
                probe.close()
        self.listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.listener.bind(path)
        self.socket_identity = os.stat(path).st_ino
        if target == 'mixer':
            # Force CC forwarding runs as admin; the compositor runs as root.
            os.chown(path, -1, grp.getgrnam('admin').gr_gid)
            os.chmod(path, 0o660)
        else:
            os.chmod(path, 0o600)
        self.listener.listen(4)
        self.listener.setblocking(False)
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def _accept_packet(self, packet):
        if not isinstance(packet, dict) or packet.get('version') != 1:
            raise ValueError('invalid protocol version')
        client, sequence = packet.get('client'), packet.get('sequence')
        if not isinstance(client, str) or len(client) != 32:
            raise ValueError('invalid client ID')
        if type(sequence) is not int or sequence < 1:
            raise ValueError('invalid sequence')
        command = validate_command(packet.get('command'), self.target)
        if sequence <= self.last_sequence.get(client, 0):
            self.replays += 1
            return {'sequence': sequence, 'status': 'accepted'}
        try:
            self.queue.put(command)
        except BufferError:
            return {'sequence': sequence, 'status': 'busy'}
        self.last_sequence[client] = sequence
        self.last_sequence.move_to_end(client)
        while len(self.last_sequence) > 128:
            self.last_sequence.popitem(last=False)
        self.accepted += 1
        return {'sequence': sequence, 'status': 'accepted'}

    def _run(self):
        clients = {}
        try:
            while not self.stop.is_set():
                ready, _, _ = select.select([self.listener] + list(clients), [], [], 0.05)
                for connection in ready:
                    if connection is self.listener:
                        client, _ = self.listener.accept()
                        client.setblocking(False)
                        if len(clients) >= 4:
                            client.close()
                        else:
                            clients[client] = bytearray()
                        continue
                    try:
                        chunk = connection.recv(MAX_PACKET + 1)
                        if not chunk:
                            raise ConnectionError('client disconnected')
                        buffer = clients[connection]
                        buffer.extend(chunk)
                        if len(buffer) > MAX_PACKET:
                            raise ValueError('packet too large')
                        while b'\n' in buffer:
                            line, _, remainder = buffer.partition(b'\n')
                            buffer[:] = remainder
                            response = self._accept_packet(json.loads(line))
                            connection.sendall(json.dumps(response).encode() + b'\n')
                    except (OSError, ValueError, TypeError, KeyError):
                        clients.pop(connection, None)
                        connection.close()
        finally:
            for connection in clients:
                connection.close()

    def drain(self, limit=CAPACITY):
        return self.queue.drain(limit)

    def close(self):
        self.stop.set()
        self.thread.join(1)
        self.listener.close()
        try:
            if os.stat(self.path).st_ino == self.socket_identity:
                os.unlink(self.path)
        except FileNotFoundError:
            pass


class CommandSender:
    """Nonblocking producer; a worker retries unacknowledged local delivery."""
    def __init__(self, path, target, capacity=CAPACITY):
        self.path, self.target = path, target
        self.queue = CommandQueue(capacity)
        self.client = uuid.uuid4().hex
        self.sequence = 0
        self.delivered = 0
        self.reconnects = 0
        self.stop = threading.Event()
        self.wake = threading.Event()
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def send(self, command):
        command = validate_command(command, self.target)
        self.queue.put(command)  # Full queues are explicit errors, never silent loss.
        self.wake.set()

    def _run(self):
        connection = None
        waiting = None
        disconnected = False
        try:
            while not self.stop.is_set():
                if waiting is None:
                    pending = self.queue.drain(1)
                    if not pending:
                        self.wake.wait(0.05)
                        self.wake.clear()
                        continue
                    self.sequence += 1
                    waiting = {'version': 1, 'client': self.client,
                               'sequence': self.sequence, 'command': pending[0]}
                try:
                    if connection is None:
                        connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                        connection.settimeout(0.5)
                        connection.connect(self.path)
                        self.reconnects += 1
                    connection.sendall(json.dumps(waiting).encode() + b'\n')
                    reply = bytearray()
                    while not reply.endswith(b'\n'):
                        chunk = connection.recv(MAX_PACKET)
                        if not chunk:
                            raise ConnectionError('receiver disconnected')
                        reply.extend(chunk)
                        if len(reply) > MAX_PACKET:
                            raise ValueError('acknowledgment too large')
                    response = json.loads(reply)
                    if response.get('sequence') != waiting['sequence']:
                        raise ValueError('acknowledgment sequence mismatch')
                    if response.get('status') == 'busy':
                        self.stop.wait(0.01)
                        continue
                    if response.get('status') != 'accepted':
                        raise ValueError('invalid acknowledgment')
                    self.delivered += 1
                    waiting = None
                    if disconnected:
                        LOG.info('Command delivery recovered: %s', self.target)
                    disconnected = False
                except (OSError, ValueError):
                    if connection is not None:
                        connection.close()
                        connection = None
                    if not disconnected:
                        LOG.warning('Command receiver unavailable; retaining %s commands', self.target)
                    disconnected = True
                    self.stop.wait(0.05)
        finally:
            if connection is not None:
                connection.close()

    def close(self):
        self.stop.set()
        self.wake.set()
        self.thread.join(1)
