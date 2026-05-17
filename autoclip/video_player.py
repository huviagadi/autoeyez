"""
video_player.py — MP4 Clip Playback Engine for autoclip
Part of the autoeyez video synthesis system

Manages clip playback via ffmpeg subprocess:
  - Plays MP4 clips from /boot/firmware/clips/
  - Supports play, pause, loop, seek, clip switching
  - Outputs raw frames to Mixer via get_frame()

IPC: Polls /tmp/player_cmd.json, writes /tmp/player_state.json
"""

import subprocess
import os
import time
import json
import numpy as np
import threading

CLIPS_DIR  = '/boot/firmware/clips/'
STATE_FILE = '/tmp/player_state.json'
CMD_FILE   = '/tmp/player_cmd.json'

W, H = 720, 576

def get_clips():
    return sorted([f for f in os.listdir(CLIPS_DIR) if f.endswith('.mp4')])

class VideoPlayer:
    def __init__(self):
        self.clips = get_clips()
        self.index = 0
        self.paused = False
        self.pause_pos = 0
        self.loop = False
        self.process = None
        self.current_frame = np.zeros((H, W, 3), dtype=np.uint8)
        self.frame_lock = threading.Lock()
        self.time_pos = 0
        self.duration = 0
        self.start_time = None
        self.write_state()

    def write_state(self):
        try:
            with open(STATE_FILE, 'w') as f:
                json.dump({
                    'index': self.index,
                    'paused': self.paused,
                    'loop': self.loop,
                    'clip': self.clips[self.index] if self.clips else '',
                    'time_pos': self.time_pos,
                    'duration': self.duration
                }, f)
        except:
            pass

    def get_duration(self, clip_path):
        try:
            result = subprocess.run(
                ['ffprobe', '-v', 'error', '-show_entries', 'format=duration',
                 '-of', 'default=noprint_wrappers=1:nokey=1', clip_path],
                capture_output=True, text=True)
            return float(result.stdout.strip())
        except:
            return 0

    def play_current(self, seek=0):
        self.stop()
        if not self.clips:
            return
        clip = os.path.join(CLIPS_DIR, self.clips[self.index])
        self.duration = self.get_duration(clip)
        self.time_pos = seek
        self.start_time = time.time() - seek
        cmd = [
            'ffmpeg', '-re',
        ]
        if seek > 0:
            cmd += ['-ss', str(seek)]
        cmd += [
            '-i', clip,
            '-vf', 'scale={}:{}'.format(W, H),
            '-pix_fmt', 'rgb24',
            '-f', 'rawvideo', 'pipe:1'
        ]
        self.process = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL
        )
        threading.Thread(target=self._read_frames, daemon=True).start()
        self.write_state()

    def _read_frames(self):
        frame_size = W * H * 3
        proc = self.process
        while proc and proc.poll() is None:
            try:
                raw = proc.stdout.read(frame_size)
                if len(raw) == frame_size:
                    frame = np.frombuffer(raw, dtype=np.uint8).reshape((H, W, 3))
                    with self.frame_lock:
                        self.current_frame = frame.copy()
                elif len(raw) == 0:
                    break
            except:
                break

    def get_frame(self):
        with self.frame_lock:
            return self.current_frame.copy()

    def stop(self):
        if self.process:
            self.process.terminate()
            try:
                self.process.wait(timeout=2)
            except:
                self.process.kill()
            self.process = None

    def next(self):
        self.index = (self.index + 1) % len(self.clips)
        self.pause_pos = 0
        self.play_current()

    def prev(self):
        self.index = (self.index - 1) % len(self.clips)
        self.pause_pos = 0
        self.play_current()

    def play_index(self, i):
        self.index = i % len(self.clips)
        self.play_current()
        self.pause_pos = 0

    def handle_cmd(self, cmd):
        action = cmd.get('action')
        if action == 'next':
            self.next()
        elif action == 'prev':
            self.prev()
        elif action == 'play_index':
            self.play_index(cmd.get('index', 0))
        elif action == 'pause':
            self.paused = True
            self.pause_pos = self.time_pos  # save position
            self.stop()
            self.write_state()
        elif action == 'play':
            self.paused = False
            self.play_current(seek=self.pause_pos)
        elif action == 'loop_on':
            self.loop = True
            self.write_state()
        elif action == 'loop_off':
            self.loop = False
            self.write_state()

    def run(self):
        self.play_current()
        last_time_write = 0
        while True:
            if os.path.exists(CMD_FILE):
                try:
                    with open(CMD_FILE) as f:
                        cmd = json.load(f)
                    os.remove(CMD_FILE)
                    self.handle_cmd(cmd)
                except:
                    pass

            if not self.paused and self.process and self.process.poll() is not None:
                if self.loop:
                    self.play_current()
                else:
                    self.next()

            if not self.paused and self.start_time:
                now = time.time()
                if now - last_time_write >= 1.0:
                    self.time_pos = min(int(now - self.start_time), int(self.duration))
                    self.write_state()
                    last_time_write = now

            time.sleep(0.1)

if __name__ == '__main__':
    player = VideoPlayer()
    player.run()
