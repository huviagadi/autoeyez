"""Focused checks for the Force cue/seek fast path without Pi-only media modules."""

import ast
import json
from pathlib import Path
import select
import socket
import threading
import time
import unittest


source = ast.parse((Path(__file__).parent / 'live_20261016_stutter' / 'video_player.py').read_text())
player_class = next(node for node in source.body
                    if isinstance(node, ast.ClassDef) and node.name == 'VideoPlayer')
namespace = dict(json=json, select=select, time=time)
exec(compile(ast.Module(body=[player_class], type_ignores=[]), '<VideoPlayer>', 'exec'), namespace)
VideoPlayer = namespace['VideoPlayer']


class ClipMidiLatencyTests(unittest.TestCase):
    def player(self):
        player = object.__new__(VideoPlayer)
        player.clips = ['one.mp4', 'two.mp4']
        player.index = 0
        player.container = object()
        player.duration = 10.0
        player.seek_lock = threading.Lock()
        player.seek_request = None
        player.seek_started_at = None
        player.pending_event = None
        player.current_event = None
        player.frame_lock = threading.Lock()
        player.stutter_lengths = {'normal': 3, 'reverse': 0}
        player.stutter_indices = {'normal': 1, 'reverse': 0}
        player.stutter_buffer = [object()]
        player.stutter_release = []
        player.stutter_replaying = True
        player.write_state = lambda: None
        return player

    def test_same_clip_cue_rewinds_without_reopening(self):
        player = self.player()
        player.open_clip = lambda: self.fail('same clip was reopened')
        player.handle_cmd({'action': 'play_index', 'index': 0,
                           'event_id': 'cue', 'sent_ns': 123})
        self.assertEqual(player.seek_request, 0.0)
        self.assertEqual(player.pending_event['event_id'], 'cue')
        self.assertEqual(player.stutter_lengths['normal'], 0)

    def test_new_clip_opens_once_and_seeks_to_final_position(self):
        player = self.player()
        opens = []
        player.open_clip = lambda: opens.append(player.index)
        player.handle_cmd({'action': 'play_index', 'index': 1,
                           'position': 0.65, 'event_id': 'seek', 'sent_ns': 456})
        self.assertEqual(opens, [1])
        self.assertEqual(player.seek_request, 0.65)
        self.assertEqual(player.pending_event['event_id'], 'seek')

    def test_datagram_burst_coalesces_cue_and_latest_seek(self):
        player = self.player()
        receiver, sender = socket.socketpair(socket.AF_UNIX, socket.SOCK_DGRAM)
        try:
            receiver.setblocking(False)
            player.sock = receiver
            opens = []
            player.open_clip = lambda: opens.append(player.index)
            for message in ({'action': 'play_index', 'index': 1,
                             'event_id': 'cue', 'sent_ns': 100},
                            {'position': 0.4, 'event_id': 'seek1', 'sent_ns': 101},
                            {'position': 0.7, 'event_id': 'seek2', 'sent_ns': 102}):
                sender.send(json.dumps(message).encode())
            player._handle_seek_datagrams()
            self.assertEqual(opens, [1])
            self.assertEqual(player.seek_request, 0.7)
            self.assertEqual(player.pending_event['event_id'], 'seek2')
        finally:
            receiver.close()
            sender.close()

    def test_seek_before_cue_has_priority_over_rewind(self):
        player = self.player()
        receiver, sender = socket.socketpair(socket.AF_UNIX, socket.SOCK_DGRAM)
        try:
            receiver.setblocking(False)
            player.sock = receiver
            player.open_clip = lambda: self.fail('same clip was reopened')
            sender.send(json.dumps({'position': 0.48, 'event_id': 'seek',
                                    'sent_ns': 101}).encode())
            sender.send(json.dumps({'action': 'play_index', 'index': 0,
                                    'event_id': 'cue', 'sent_ns': 102}).encode())
            player._handle_seek_datagrams()
            self.assertEqual(player.seek_request, 0.48)
            self.assertEqual(player.pending_event['event_id'], 'seek')
        finally:
            receiver.close()
            sender.close()

    def test_combined_cue_packet_keeps_position(self):
        player = self.player()
        receiver, sender = socket.socketpair(socket.AF_UNIX, socket.SOCK_DGRAM)
        try:
            receiver.setblocking(False)
            player.sock = receiver
            player.open_clip = lambda: self.fail('same clip was reopened')
            sender.send(json.dumps({'action': 'play_index', 'index': 0,
                                    'position': 0.48, 'event_id': 'combined',
                                    'sent_ns': 101}).encode())
            player._handle_seek_datagrams()
            self.assertEqual(player.seek_request, 0.48)
            self.assertEqual(player.pending_event['event_id'], 'combined')
        finally:
            receiver.close()
            sender.close()

    def test_marker_copies_read_only_converted_frame(self):
        player = self.player()
        player.marker_frames_remaining = 1
        player.marker_value = 0xA5
        class Frame:
            def __init__(self, writable):
                self.flags = type('Flags', (), {'writeable': writable})()
                self.writes = []
            def copy(self):
                return Frame(True)
            def __setitem__(self, key, value):
                if not self.flags.writeable:
                    raise ValueError('assignment destination is read-only')
                self.writes.append((key, value))
        source = Frame(False)
        output = player._stamp_latency_marker(source)
        self.assertIsNot(output, source)
        self.assertEqual(len(output.writes), 12)
        self.assertEqual(player.marker_frames_remaining, 0)

    def test_socket_wait_wakes_before_source_frame_deadline(self):
        receiver, sender = socket.socketpair(socket.AF_UNIX, socket.SOCK_DGRAM)
        try:
            def send():
                time.sleep(0.01)
                sender.send(b'{}')
            worker = threading.Thread(target=send)
            worker.start()
            start = time.perf_counter()
            readable, _, _ = select.select([receiver], [], [], 0.08)
            elapsed = time.perf_counter() - start
            worker.join()
            self.assertTrue(readable)
            self.assertLess(elapsed, 0.06)
        finally:
            receiver.close()
            sender.close()


if __name__ == '__main__':
    unittest.main()
