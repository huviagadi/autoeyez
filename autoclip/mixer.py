"""
mixer.py — Video Compositor for autoclip
Part of the autoeyez video synthesis system

Main process that:
  - Captures 3 composite video sources via USB capture cards
  - Composites with crossfade and luma keying
  - Writes to local framebuffer (/dev/fb0)
  - Streams to autowaaave via TCP

Run via systemd: video.service (as root for framebuffer access)
IPC: Polls /tmp/mixer_cmd.json for commands from video_control.py
"""

import subprocess
import numpy as np
import cv2
import threading
import time
import os
import json
import sys

W, H = 720, 576
FBDEV = '/dev/fb0'
STATE_FILE   = '/tmp/mixer_state.json'
CMD_FILE     = '/tmp/mixer_cmd.json'

# Source indices
SRC_CLIPS        = 0
SRC_COMPOSITE1   = 1
SRC_COMPOSITE2   = 2
SRC_COMPOSITE3   = 3  # dummy
SRC_OSCILLOSCOPE = 4

CAPTURE_DEVICES = [
    {'device': '/dev/video0', 'fmt': 'MJPG', 'w': 640, 'h': 480, 'pad': True},
    {'device': '/dev/video2', 'fmt': 'MJPG', 'w': 640, 'h': 480, 'pad': True},
    {'device': '/dev/video4', 'fmt': 'MJPG', 'w': 640, 'h': 480, 'pad': True},
]

# ============================================================
# CAPTURE CHANNEL
# ============================================================

class CaptureChannel:
    def __init__(self, device, fmt, w=720, h=576, pad=False):
        self.device  = device
        self.fmt     = fmt
        self.cw      = w
        self.ch      = h
        self.pad     = pad
        self.frame   = np.zeros((H, W, 3), dtype=np.uint8)
        self.lock    = threading.Lock()
        self.running = False
        self.process = None
        self.last_frame_time = time.time() + 10
        self.first_frame = False

    def start(self):
        self.running = True
        threading.Thread(target=self._capture, daemon=True).start()

    def _capture(self):
        while self.running:
            try:
                start_time = time.time()
                fmt_map = {'MJPG': 'mjpeg', 'YUYV': 'yuyv422'}
                input_fmt = fmt_map.get(self.fmt)
                fps = '15' if self.fmt == 'YUYV' else '30'
                cmd = ['ffmpeg', '-loglevel', 'error',
                       '-f', 'v4l2']
                if input_fmt:
                    cmd += ['-input_format', input_fmt]
                cmd += [
                    '-video_size', '{}x{}'.format(self.cw, self.ch),
                    '-framerate', fps,
                    '-i', self.device,
                ]
                if self.pad:
                    vf = 'scale={}:{}'.format(W, H)
                else:
                    vf = 'scale={}:{}'.format(W, H)
                cmd += [
                    '-vf', vf,
                    '-pix_fmt', 'rgb24',
                    '-f', 'rawvideo',
                    'pipe:1'
                ]
                self.process = subprocess.Popen(
                    cmd,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.DEVNULL
                )
                frame_size = W * H * 3
                frame_count = 0
                while self.running:
                    raw = self.process.stdout.read(frame_size)
                    if len(raw) == frame_size:
                        frame = np.frombuffer(raw, dtype=np.uint8).reshape((H, W, 3))
                        with self.lock:
                            self.frame = frame.copy()
                        frame_count += 1
                        self.last_frame_time = time.time()
                        self.first_frame = True
                        if frame_count == 1:
                            print("First frame received: {}".format(self.device), flush=True)
                    elif len(raw) == 0:
                        print("Pipe closed: {}".format(self.device), flush=True)
                        break
                    else:
                        print("Short read {} bytes: {}".format(len(raw), self.device), flush=True)
            except Exception as e:
                print("Capture error {}: {}".format(self.device, e))
            finally:
                elapsed = time.time() - start_time
                if self.process:
                    self.process.terminate()
                    self.process = None
                if elapsed < 3:
                    print("Fast fail on {} ({:.1f}s), backing off".format(self.device, elapsed), flush=True)
                    time.sleep(15)
                else:
                    time.sleep(1)

    def get_frame(self):
        with self.lock:
            return self.frame

# ============================================================
# OSCILLOSCOPE CHANNEL
# ============================================================

class OscilloscopeChannel:
    def __init__(self):
        self.frame   = np.zeros((H, W, 3), dtype=np.uint8)
        self.lock    = threading.Lock()
        self.running = False
        self.t       = 0.0

    def start(self):
        self.running = True
        threading.Thread(target=self._generate, daemon=True).start()

    def _generate(self):
        try:
            import sounddevice as sd
            buf_size = 512
            buf = np.zeros((buf_size, 2), dtype=np.float32)
            buf_lock = threading.Lock()

            def audio_callback(indata, frames, time_info, status):
                with buf_lock:
                    buf[:] = indata[:buf_size]

            stream = sd.InputStream(
                device=1,
                samplerate=48000,
                channels=2,
                blocksize=buf_size,
                callback=audio_callback
            )
            stream.start()

            while self.running:
                with buf_lock:
                    x_chan = buf[:, 0].copy()
                    y_chan = buf[:, 1].copy()

                with self.lock:
                    frame = (self.frame.astype(np.float32) * 0.4).astype(np.uint8)

                cx, cy = W // 2, H // 2
                scale_x = (W // 2 - 10)
                scale_y = (H // 2 - 10)

                x_norm = x_chan - np.mean(x_chan)
                y_norm = y_chan - np.mean(y_chan)

                x_max = np.max(np.abs(x_norm)) + 1e-6
                y_max = np.max(np.abs(y_norm)) + 1e-6
                x_norm = x_norm / x_max
                y_norm = y_norm / y_max

                shift = len(x_norm) // 4
                y_shifted = np.roll(y_norm, shift)

                step = max(1, len(x_norm) // 256)
                pts = []
                for i in range(0, len(x_norm), step):
                    px = int(cx + x_norm[i] * scale_x)
                    py = int(cy - y_shifted[i] * scale_y)
                    px = np.clip(px, 0, W - 1)
                    py = np.clip(py, 0, H - 1)
                    pts.append((px, py))

                for i in range(1, len(pts)):
                    cv2.line(frame, pts[i-1], pts[i], (0, 255, 0), 1, cv2.LINE_AA)

                with self.lock:
                    self.frame = frame
                time.sleep(1.0 / 50.0)

            stream.stop()

        except Exception as e:
            print("Oscilloscope error: {}".format(e))
            while self.running:
                frame = np.zeros((H, W, 3), dtype=np.uint8)
                cx, cy = W // 2, H // 2
                sx, sy = W // 2 - 10, H // 2 - 10
                self.t += 0.02
                pts = []
                for i in range(512):
                    angle = i * np.pi * 2 / 512
                    px = int(cx + np.sin(angle * 3 + self.t) * sx)
                    py = int(cy + np.sin(angle * 2) * sy)
                    pts.append((np.clip(px, 0, W-1), np.clip(py, 0, H-1)))
                for i in range(1, len(pts)):
                    cv2.line(frame, pts[i-1], pts[i], (0, 255, 0), 1, cv2.LINE_AA)
                with self.lock:
                    self.frame = frame
                time.sleep(1.0 / 15.0)

    def get_frame(self):
        with self.lock:
            return self.frame.copy()

# ============================================================
# MIXER
# ============================================================

class Mixer:
    def __init__(self, clip_player):
        self.stream_process = None
        self._start_stream()
        self.clip_player = clip_player

        self.captures = [CaptureChannel(**d) for d in CAPTURE_DEVICES]
        self.dummy    = np.zeros((H, W, 3), dtype=np.uint8)
        self.osc      = OscilloscopeChannel()

        self.ch_a      = SRC_CLIPS
        self.ch_b      = SRC_COMPOSITE1
        self.mix       = 0
        self.luma      = 0
        self.luma_src  = 0
        self.luma_high         = 127
        self.luma_high_enabled = False
        self.luma_out  = np.empty((H, W, 3), dtype=np.uint8)
        self.blend_out = np.empty((H, W, 3), dtype=np.uint8)

        self.fb = open(FBDEV, 'wb')
        self.out_buf = np.empty((H, W, 4), dtype=np.uint8)
        self.write_state()

    def write_state(self):
        try:
            with open(STATE_FILE, 'w') as f:
                json.dump({
                    'ch_a': self.ch_a,
                    'ch_b': self.ch_b,
                    'mix': self.mix,
                    'luma': self.luma,
                    'luma_src': self.luma_src,
                    'luma_high': self.luma_high,
                    'luma_high_enabled': self.luma_high_enabled,
                }, f)
        except:
            pass

    def get_source_frame(self, src):
        if src == SRC_CLIPS:
            return self.clip_player.get_frame()
        elif src == SRC_COMPOSITE1:
            return self.captures[0].get_frame()
        elif src == SRC_COMPOSITE2:
            return self.captures[1].get_frame()
        elif src == SRC_COMPOSITE3:
            return self.captures[2].get_frame()
        elif src == SRC_OSCILLOSCOPE:
            return self.dummy.copy()
        return self.dummy.copy()

    def handle_cmd(self, cmd):
        action = cmd.get('action')
        if action == 'ch_a':
            self.ch_a = cmd.get('value', 0)
        elif action == 'ch_b':
            self.ch_b = cmd.get('value', 1)
        elif action == 'mix':
            self.mix = cmd.get('value', 0)
        elif action == 'luma':
            self.luma = cmd.get('value', 0)
        elif action == 'luma_src':
            self.luma_src = cmd.get('value', 0)
        elif action == 'luma_high':
            val = cmd.get('value', 127)
            self.luma_high = max(val, self.luma)  # can't go below low
            self.write_state()
        elif action == 'luma_high_en':
            self.luma_high_enabled = cmd.get('value', False)
            self.write_state()
        elif action == 'luma':
            self.luma = min(cmd.get('value', 0), self.luma_high)  # can't go above high
            self.write_state()
        self.write_state()

    def composite(self):
        # Only copy frames we actually need
        need_a = True
        need_b = self.mix > 0 or self.luma > 0

        frame_a = self.get_source_frame(self.ch_a) if need_a else self.dummy
        frame_b = self.get_source_frame(self.ch_b) if need_b else self.dummy

        # Fast paths — no blend, no luma
        if self.mix == 0 and self.luma == 0:
            return frame_a

        if self.mix == 127 and self.luma == 0:
            return frame_b

        # Blend
        if self.mix > 0:
            mix_f = self.mix / 127.0
            cv2.addWeighted(frame_a, 1.0 - mix_f, frame_b, mix_f, 0, dst=self.blend_out)
            out = self.blend_out
        else:
            out = frame_a

        # Luma key
        if self.luma > 0 and self.luma_src > 0:
            key_frame = self.get_source_frame(self.luma_src - 1)
            gray = cv2.cvtColor(key_frame, cv2.COLOR_RGB2GRAY)
            luma_low_u8  = int(self.luma * 2)
            luma_high_u8 = int(self.luma_high * 2)

            if self.luma_high_enabled:
                # Band key — show key_frame only between low and high thresholds
                _, mask_low  = cv2.threshold(gray, luma_low_u8,  255, cv2.THRESH_BINARY)
                _, mask_high = cv2.threshold(gray, luma_high_u8, 255, cv2.THRESH_BINARY)
                mask = cv2.bitwise_and(mask_low, cv2.bitwise_not(mask_high))
            else:
                # Low key only
                _, mask = cv2.threshold(gray, luma_low_u8, 255, cv2.THRESH_BINARY)

            out = np.where(mask[:,:,np.newaxis] > 0, key_frame, out)

        return out

    def write_frame(self, frame):
        self.out_buf[:,:,0] = frame[:,:,2]
        self.out_buf[:,:,1] = frame[:,:,1]
        self.out_buf[:,:,2] = frame[:,:,0]
        self.out_buf[:,:,3] = 255
        self.fb.write(self.out_buf.tobytes())
        self.fb.seek(0)
        if self.stream_process and self.stream_process.poll() is None:
            try:
                self.stream_process.stdin.write(frame.tobytes())
            except:
                pass

    def start(self):
        time.sleep(5)
        for i, ch in enumerate(self.captures):
            ch.start()
            deadline = time.time() + 10
            while not ch.first_frame and time.time() < deadline:
                time.sleep(0.5)
            if ch.first_frame:
                print("Capture confirmed: {}".format(ch.device), flush=True)
            else:
                print("Capture timeout: {}".format(ch.device), flush=True)
            time.sleep(1)
        # self.osc.start()
        threading.Thread(target=self._watchdog, daemon=True).start()
        threading.Thread(target=self._cmd_thread, daemon=True).start()

    def _watchdog(self):
        while True:
            time.sleep(5)
            for ch in self.captures:
                age = time.time() - ch.last_frame_time
                threshold = 8 if not ch.first_frame else 15
                if age > threshold:
                    print("Watchdog restarting capture: {} (first_frame={})".format(
                        ch.device, ch.first_frame), flush=True)
                    ch.first_frame = False
                    if ch.process:
                        ch.process.terminate()
                        ch.process = None
                    ch.last_frame_time = time.time() + 20

    def _cmd_thread(self):
        while True:
            if os.path.exists(CMD_FILE):
                try:
                    with open(CMD_FILE) as f:
                        cmd = json.load(f)
                    os.remove(CMD_FILE)
                    self.handle_cmd(cmd)
                except:
                    pass
            time.sleep(0.01)

    def run(self):
        self.start()
        target_fps = 30
        frame_time = 1.0 / target_fps
        while True:
            t0 = time.time()

            frame = self.composite()
            self.write_frame(frame)

            elapsed = time.time() - t0
            sleep_t = frame_time - elapsed
            if sleep_t > 0:
                time.sleep(sleep_t)
            else:
                print("Frame overrun: {:.1f}ms".format(elapsed * 1000), flush=True)

    def _start_stream(self):
        def _connect():
            while True:
                cmd = [
                    'ffmpeg', '-loglevel', 'quiet',
                    '-f', 'rawvideo', '-pix_fmt', 'rgb24',
                    '-video_size', '{}x{}'.format(W, H),
                    '-framerate', '30',
                    '-i', 'pipe:0',
                    '-vf', 'scale=720:480',
                    '-pix_fmt', 'yuv420p',
                    '-c:v', 'libx264',
                    '-preset', 'ultrafast',
                    '-tune', 'zerolatency',
                    '-crf', '18',
                    '-f', 'mpegts',
                    'tcp://10.0.0.1:1236'
                ]
                self.stream_process = subprocess.Popen(
                    cmd, stdin=subprocess.PIPE,
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
                )
                self.stream_process.wait()
                print("Stream disconnected, retrying...", flush=True)
                time.sleep(2)
        threading.Thread(target=_connect, daemon=True).start()
        print("Stream started", flush=True)

# ============================================================
# ENTRY POINT
# ============================================================

if __name__ == '__main__':
    sys.path.insert(0, '/home/admin')
    from video_player import VideoPlayer

    player = VideoPlayer()
    threading.Thread(target=player.run, daemon=True).start()
    time.sleep(2)

    mixer = Mixer(player)
    mixer.run()
