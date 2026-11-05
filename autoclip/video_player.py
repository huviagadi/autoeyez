#!/usr/bin/env python3
"""
video_player.py — PyAV-based Clip Playback for autoclip
With event-driven frame handoff for reduced latency.
"""

import os
import time
import json
import socket
import threading
import numpy as np
import av

CLIPS_DIR = '/home/admin/clips/'
SEEK_CLIPS_DIR = '/home/admin/clips_seek/'
STATE_FILE = '/tmp/player_state.json'
CMD_FILE = '/tmp/player_cmd.json'
SEEK_SOCKET = '/tmp/player_seek.sock'

W, H = 720, 576
DEFAULT_CLIP = 'LIGHTWALL'
TARGET_FPS = 30
FRAME_TIME = 1.0 / TARGET_FPS

def get_clips():
    if not os.path.exists(CLIPS_DIR):
        return []
    return sorted([f for f in os.listdir(CLIPS_DIR) if f.endswith('.mp4')])

class VideoPlayer:
    def __init__(self):
        self.clips = get_clips()
        self.index = self._find_default()
        self.paused = False
        self.loop = False
        
        self.current_frame = np.zeros((H, W, 3), dtype=np.uint8)
        self.frame_lock = threading.Lock()
        self.frame_ready = threading.Event()  # NEW: signal when frame is ready
        
        self.time_pos = 0.0
        self.duration = 0.0
        self.frame_period = 1.0/30
        self.next_frame_time = time.perf_counter()
        self.container = None
        self.stream = None
        
        self.running = True
        self.seek_request = None
        self.seek_decode_target = None
        self.seek_started_at = None
        self.seek_lock = threading.Lock()
        self.stutter_start = 0.0
        self.stutter_lengths = {'normal': 0, 'reverse': 0}
        self.stutter_indices = {'normal': 0, 'reverse': 0}
        self.stutter_last_control = 'normal'
        self.stutter_buffer = []
        self.stutter_release = []
        self.stutter_replaying = False
        
        self._init_socket()
        self.write_state()
    
    def _init_socket(self):
        if os.path.exists(SEEK_SOCKET):
            os.remove(SEEK_SOCKET)
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.sock.bind(SEEK_SOCKET)
        self.sock.setblocking(False)
        os.chmod(SEEK_SOCKET, 0o666)
    
    def _find_default(self):
        for i, c in enumerate(self.clips):
            if DEFAULT_CLIP.upper() in c.upper():
                return i
        return 0
    
    def write_state(self):
        try:
            with open(STATE_FILE, 'w') as f:
                json.dump({
                    'index': self.index,
                    'paused': self.paused,
                    'loop': self.loop,
                    'clip': self.clips[self.index] if self.clips else '',
                    'time_pos': self.time_pos,
                    'duration': self.duration,
                    'frame_period': self.frame_period,
                    'stutter': max(self.stutter_lengths.values()),
                    'stutter_controls': self.stutter_lengths
                }, f)
        except:
            pass
    
    def open_clip(self):
        self.seek_decode_target = None
        self.stutter_lengths = {'normal': 0, 'reverse': 0}
        self.stutter_indices = {'normal': 0, 'reverse': 0}
        self.stutter_buffer = []
        self.stutter_release = []
        self.stutter_replaying = False
        if self.container:
            self.container.close()
            self.container = None
        
        if not self.clips:
            return
        
        source_path = os.path.join(CLIPS_DIR, self.clips[self.index])
        seek_path = os.path.join(SEEK_CLIPS_DIR, self.clips[self.index])
        path = seek_path if os.path.exists(seek_path) else source_path
        try:
            self.container = av.open(path)
            self.stream = self.container.streams.video[0]
            self.stream.thread_type = 'AUTO'
            self.duration = float(self.stream.duration * self.stream.time_base) if self.stream.duration else 0
            self.frame_period = 1.0 / float(self.stream.average_rate) if self.stream.average_rate else 1.0/30
            self.next_frame_time = time.perf_counter()
            self.time_pos = 0.0
            source_kind = 'seek' if path == seek_path else 'original'
            print(f'Opened: {self.clips[self.index]} ({self.duration:.1f}s, {source_kind})', flush=True)
        except Exception as e:
            print(f'Failed to open: {e}', flush=True)
            self.container = None
    
    def seek(self, position):
        if self.container and self.duration > 0:
            with self.seek_lock:
                self.seek_request = position
                self.seek_started_at = time.perf_counter()
    
    def _do_seek(self, position):
        target_sec = min(max(0.0, position * self.duration),
                         max(0.0, self.duration - self.frame_period))
        target_pts = int(target_sec / self.stream.time_base)
        try:
            self.container.seek(target_pts, stream=self.stream)
            # seek() lands on the preceding keyframe. Decode forward before
            # displaying a frame so note velocity retains its fine resolution.
            self.seek_decode_target = target_sec
            self.stutter_start = target_sec
            self.stutter_buffer = []
            self.stutter_release = []
            self.stutter_indices = {'normal': 0, 'reverse': 0}
            self.stutter_replaying = False
            print(f'Seek: {position*100:.0f}% -> {target_sec:.2f}s', flush=True)
        except Exception as e:
            print(f'Seek failed: {e}', flush=True)

    def set_stutter(self, value, control='normal'):
        """Set one lane of the shared two-head 30-frame stutter buffer."""
        if control not in self.stutter_lengths:
            return
        value = max(0, min(30, int(value)))
        was_active = any(self.stutter_lengths.values())
        if value == 0:
            released_index = self.stutter_indices[control]
            self.stutter_lengths[control] = 0
            if was_active and not any(self.stutter_lengths.values()):
                if self.stutter_replaying:
                    start = min(released_index, len(self.stutter_buffer))
                    self.stutter_release = self.stutter_buffer[start:]
                self.stutter_buffer = []
                self.stutter_indices = {'normal': 0, 'reverse': 0}
                self.stutter_replaying = False
                print('Stutter off', flush=True)
            else:
                print('Stutter {} lane off'.format(control), flush=True)
            return

        if not was_active:
            self.stutter_start = max(0.0, min(self.time_pos, self.duration))
            self.stutter_buffer = []
            self.stutter_indices = {'normal': 0, 'reverse': 0}
            self.stutter_release = []
            self.stutter_replaying = False
        self.stutter_lengths[control] = value
        self.stutter_last_control = control
        if len(self.stutter_buffer) < max(self.stutter_lengths.values()):
            self.stutter_replaying = False
        print('Stutter {}: start={:.3f}s length={} frames ({:.3f}s)'.format(
            control, self.stutter_start, value, value / TARGET_FPS), flush=True)
    
    def get_frame(self):
        with self.frame_lock:
            return self.current_frame.copy()
    
    def wait_frame(self, timeout=0.05):
        """Wait for next frame with timeout. Returns True if frame ready."""
        return self.frame_ready.wait(timeout)
    
    def next(self):
        self.index = (self.index + 1) % len(self.clips) if self.clips else 0
        self.open_clip()
        self.write_state()
    
    def prev(self):
        self.index = (self.index - 1) % len(self.clips) if self.clips else 0
        self.open_clip()
        self.write_state()
    
    def handle_cmd(self, cmd):
        action = cmd.get('action')
        if action == 'next':
            self.next()
        elif action == 'prev':
            self.prev()
        elif action == 'play_index':
            self.index = cmd.get('index', 0) % len(self.clips) if self.clips else 0
            self.open_clip()
            self.write_state()
        elif action == 'play_loop':
            # Restore the clip and loop setting together when loading a patch.
            self.index = cmd.get('index', 0) % len(self.clips) if self.clips else 0
            self.loop = bool(cmd.get('loop', False))
            self.paused = False
            self.open_clip()
            self.write_state()
        elif action == 'loop_on':
            self.loop = True
            self.write_state()
        elif action == 'loop_off':
            self.loop = False
            self.write_state()
        elif action == 'pause':
            self.paused = True
            self.write_state()
        elif action == 'play':
            self.paused = False
            self.write_state()
        elif action == 'cue_preview':
            # Hold the requested frame for keypad cue editing. The decode loop
            # is allowed to land one frame while paused.
            self.paused = True
            self.seek(cmd.get('position', 0.0))
            self.write_state()
    
    def run(self):
        self.open_clip()
        last_state_write = 0
        frame_count = 0
        fps_start = time.time()
        
        while self.running:
            now = time.time()
            
            # Check for seek commands via socket
            try:
                while True:
                    data = self.sock.recv(256)
                    message = json.loads(data.decode())
                    if message.get('action') == 'stutter_control':
                        self.set_stutter(message.get('value', 0),
                                         message.get('control', 'normal'))
                    elif message.get('action') == 'stutter':
                        self.set_stutter(message.get('value', 0), 'normal')
                    elif message.get('action') == 'play_index':
                        self.handle_cmd(message)
                    elif message.get('action') in ('cue_preview', 'play', 'pause'):
                        self.handle_cmd(message)
                    elif 'position' in message:
                        self.seek(message['position'])
            except BlockingIOError:
                pass
            except:
                pass
            
            # Check for file commands
            if os.path.exists(CMD_FILE):
                try:
                    with open(CMD_FILE) as f:
                        cmd = json.load(f)
                    os.remove(CMD_FILE)
                    self.handle_cmd(cmd)
                except:
                    pass
            
            # Handle seek request
            with self.seek_lock:
                if self.seek_request is not None:
                    self._do_seek(self.seek_request)
                    self.seek_request = None
            
            # Decode next frame if not paused
            if (not self.paused or self.seek_decode_target is not None) and self.container:
                replay_arr = None
                replay_pos = None
                if self.stutter_release:
                    replay_arr = self.stutter_release.pop(0)
                    replay_pos = self.time_pos + self.frame_period
                elif (any(self.stutter_lengths.values()) and
                      len(self.stutter_buffer) >= max(self.stutter_lengths.values())):
                    self.stutter_replaying = True
                    heads = []
                    positions = {}
                    for control, length in self.stutter_lengths.items():
                        if length <= 0:
                            continue
                        index = self.stutter_indices[control] % length
                        positions[control] = index
                        heads.append(self.stutter_buffer[index])
                        self.stutter_indices[control] = (index + 1) % length
                    replay_arr = heads[0]
                    if len(heads) == 2:
                        replay_arr = ((heads[0].astype(np.uint16) +
                                       heads[1].astype(np.uint16)) // 2).astype(np.uint8)
                    position_index = positions.get(
                        self.stutter_last_control, next(iter(positions.values())))
                    replay_pos = self.stutter_start + position_index / TARGET_FPS

                if replay_arr is not None:
                    with self.frame_lock:
                        self.current_frame = replay_arr
                    self.frame_ready.set()
                    self.time_pos = replay_pos
                    frame_count += 1
                    self.next_frame_time += self.frame_period
                    sleep_time = self.next_frame_time - time.perf_counter()
                    if sleep_time > 0:
                        time.sleep(sleep_time)
                    elif sleep_time < -0.1:
                        self.next_frame_time = time.perf_counter()
                    time.sleep(0.001)
                    continue

                try:
                    for frame in self.container.decode(self.stream):
                        if self.seek_decode_target is not None:
                            frame_time = (float(frame.pts * self.stream.time_base)
                                          if frame.pts is not None else None)
                            if (frame_time is not None and
                                    frame_time + self.frame_period * 0.5 < self.seek_decode_target):
                                continue
                            self.seek_decode_target = None
                            self.next_frame_time = time.perf_counter()
                            if self.seek_started_at is not None:
                                seek_ms = (time.perf_counter() - self.seek_started_at) * 1000.0
                                print(f'Seek landed: {seek_ms:.1f}ms', flush=True)
                                self.seek_started_at = None
                        arr = frame.to_ndarray(format='rgb24')
                        
                        if arr.shape[0] != H or arr.shape[1] != W:
                            from PIL import Image
                            img = Image.fromarray(arr)
                            img = img.resize((W, H), Image.BILINEAR)
                            arr = np.array(img)
                        
                        with self.frame_lock:
                            self.current_frame = arr
                        
                        # Signal frame ready
                        self.frame_ready.set()
                        
                        self.time_pos = float(frame.pts * self.stream.time_base) if frame.pts else self.time_pos
                        frame_count += 1
                        target_frames = max(self.stutter_lengths.values())
                        if (target_frames > 0 and
                                len(self.stutter_buffer) < target_frames):
                            self.stutter_buffer.append(arr.copy())
                            if len(self.stutter_buffer) == target_frames:
                                self.stutter_indices = {'normal': 0, 'reverse': 0}
                        
                        # Frame rate pacing
                        self.next_frame_time += self.frame_period
                        sleep_time = self.next_frame_time - time.perf_counter()
                        if sleep_time > 0:
                            time.sleep(sleep_time)
                        elif sleep_time < -0.1:  # Fell behind by more than 100ms
                            self.next_frame_time = time.perf_counter()
                        break
                    
                except av.error.EOFError:
                    self.seek_decode_target = None
                    if self.loop:
                        self.container.seek(0, stream=self.stream)
                        self.time_pos = 0
                    else:
                        self.next()
                except StopIteration:
                    self.seek_decode_target = None
                    if self.loop:
                        self.container.seek(0, stream=self.stream)
                        self.time_pos = 0
                    else:
                        self.next()
                except Exception as e:
                    print(f'Decode error: {e}', flush=True)
            
            # Write state periodically
            if now - last_state_write >= 1.0:
                self.write_state()
                last_state_write = now
                elapsed = now - fps_start
                if elapsed > 0:
                    fps = frame_count / elapsed
                    print(f'FPS: {fps:.1f}', flush=True)
                frame_count = 0
                fps_start = now
            
            # Minimal sleep when not decoding
            time.sleep(0.001)
    
    def stop(self):
        self.running = False
        if self.container:
            self.container.close()

if __name__ == '__main__':
    player = VideoPlayer()
    try:
        player.run()
    except KeyboardInterrupt:
        player.stop()
