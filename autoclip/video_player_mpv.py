"""
video_player.py — MPV-based Clip Playback Engine for autoclip
Part of the autoeyez video synthesis system

Uses mpv for instant seeking with raw frame output via ffmpeg filter.
MPV handles playback/seeking, outputs to pipe, we read frames for mixer.

Key advantage: seek_to() is instant - just tells mpv to seek, no restart.
"""

import os
import time
import json
import numpy as np
import threading
import subprocess

# =============================================================================
# CONFIGURATION
# =============================================================================

CLIPS_DIR = '/home/admin/clips/'
STATE_FILE = '/tmp/player_state.json'
CMD_FILE = '/tmp/player_cmd.json'
SEEK_FILE = '/tmp/player_seek.json'

W, H = 720, 576
FRAME_SIZE = W * H * 3
DEFAULT_CLIP = 'LIGHTWALL'

# =============================================================================
# UTILITY FUNCTIONS
# =============================================================================

def get_clips():
    """Get sorted list of MP4 files from clips directory."""
    if not os.path.exists(CLIPS_DIR):
        return []
    return sorted([f for f in os.listdir(CLIPS_DIR) if f.endswith('.mp4')])

# =============================================================================
# VIDEO PLAYER CLASS
# =============================================================================

class VideoPlayer:
    """
    MPV-based video player with instant seeking.

    Uses mpv subprocess with IPC socket for control.
    Outputs raw frames via ffmpeg filter for mixer.py.
    """

    def __init__(self):
        self.clips = get_clips()
        self.index = self._find_default_clip()
        self.paused = False
        self.pause_pos = 0
        self.loop = False

        # Frame buffer
        self.current_frame = np.zeros((H, W, 3), dtype=np.uint8)
        self.frame_lock = threading.Lock()

        # Playback state
        self.time_pos = 0
        self.duration = 0
        self.process = None
        self.ipc_socket = '/tmp/mpv_socket'

        # Reader thread flag
        self.reader_active = False

        self.write_state()

    def _find_default_clip(self):
        """Find the default clip index by name."""
        if not self.clips:
            return 0
        search = DEFAULT_CLIP.upper()
        for i, clip in enumerate(self.clips):
            if search in clip.upper():
                return i
        return 0

    def write_state(self):
        """Write current player state to JSON file."""
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

    def _send_mpv_command(self, cmd):
        """Send command to mpv via IPC socket."""
        import socket
        try:
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            sock.connect(self.ipc_socket)
            sock.send((json.dumps({"command": cmd}) + '\n').encode())
            response = sock.recv(4096).decode()
            sock.close()
            return json.loads(response)
        except Exception as e:
            return None

    def _get_mpv_property(self, prop):
        """Get property from mpv."""
        result = self._send_mpv_command(["get_property", prop])
        if result and 'data' in result:
            return result['data']
        return None

    def seek_to(self, position):
        """
        Seek to a position in the clip (0.0 to 1.0).
        INSTANT - just sends seek command to mpv.
        """
        if self.duration > 0:
            seek_time = position * self.duration
            self._send_mpv_command(["seek", seek_time, "absolute"])
            self.time_pos = seek_time
            print(f"SEEK: {position*100:.1f}% -> {seek_time:.2f}s", flush=True)

    def play_current(self, seek=0):
        """Start playback of the current clip."""
        self.stop()

        if not self.clips:
            return

        clip = os.path.join(CLIPS_DIR, self.clips[self.index])

        # Get duration first
        self.duration = self._get_duration(clip)

        # Build mpv command with raw video output
        # mpv decodes, applies filters, outputs raw RGB to stdout
        cmd = [
            'mpv',
            '--no-terminal',
            '--input-ipc-server=' + self.ipc_socket,
            '--vo=null',  # No display output
            '--ao=null',  # No audio output
            '--vf=format=rgb24,scale={}:{}'.format(W, H),
            '--of=rawvideo',
            '--ovc=rawvideo',
            '-o', '-',  # Output to stdout
        ]

        if self.loop:
            cmd.append('--loop-file=inf')

        if seek > 0:
            cmd.append('--start={}'.format(seek))

        cmd.append(clip)

        # Start mpv process
        self.process = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL
        )

        # Start frame reader thread
        self.reader_active = True
        threading.Thread(target=self._read_frames, daemon=True).start()

        self.paused = False
        self.time_pos = seek
        self.write_state()
        print(f"Playing: {self.clips[self.index]}", flush=True)

    def _get_duration(self, clip_path):
        """Get duration of a video file using ffprobe."""
        try:
            result = subprocess.run(
                ['ffprobe', '-v', 'error', '-show_entries', 'format=duration',
                 '-of', 'default=noprint_wrappers=1:nokey=1', clip_path],
                capture_output=True, text=True)
            return float(result.stdout.strip())
        except:
            return 0

    def _read_frames(self):
        """Background thread: read raw frames from mpv stdout."""
        proc = self.process

        while self.reader_active and proc and proc.poll() is None:
            try:
                raw = proc.stdout.read(FRAME_SIZE)
                if len(raw) == FRAME_SIZE:
                    frame = np.frombuffer(raw, dtype=np.uint8).reshape((H, W, 3))
                    with self.frame_lock:
                        self.current_frame = frame.copy()
                elif len(raw) == 0:
                    break
            except:
                break

    def get_frame(self):
        """Get the current video frame (thread-safe)."""
        with self.frame_lock:
            return self.current_frame.copy()

    def stop(self):
        """Stop playback."""
        self.reader_active = False
        if self.process:
            self.process.terminate()
            try:
                self.process.wait(timeout=2)
            except:
                self.process.kill()
            self.process = None
        # Clean up socket
        try:
            os.remove(self.ipc_socket)
        except:
            pass

    def next(self):
        """Advance to the next clip."""
        self.index = (self.index + 1) % len(self.clips)
        self.pause_pos = 0
        self.play_current()

    def prev(self):
        """Go to the previous clip."""
        self.index = (self.index - 1) % len(self.clips)
        self.pause_pos = 0
        self.play_current()

    def play_index(self, i, force=False):
        """Jump to a specific clip by index."""
        new_index = i % len(self.clips)
        if new_index != self.index or force:
            self.index = new_index
            self.play_current()
            self.pause_pos = 0

    def handle_cmd(self, cmd):
        """Execute a command received via IPC."""
        action = cmd.get('action')

        if action == 'next':
            self.next()
        elif action == 'prev':
            self.prev()
        elif action == 'play_index':
            self.play_index(cmd.get('index', 0))
        elif action == 'play_loop':
            self.loop = cmd.get('loop', False)
            self.play_index(cmd.get('index', 0))
            self.write_state()
        elif action == 'pause':
            self.paused = True
            self.pause_pos = self.time_pos
            self._send_mpv_command(["set_property", "pause", True])
            self.write_state()
        elif action == 'play':
            self.paused = False
            self._send_mpv_command(["set_property", "pause", False])
        elif action == 'loop_on':
            self.loop = True
            self._send_mpv_command(["set_property", "loop-file", "inf"])
            self.write_state()
        elif action == 'loop_off':
            self.loop = False
            self._send_mpv_command(["set_property", "loop-file", "no"])
            self.write_state()

    def run(self):
        """Main run loop."""
        self.play_current()
        last_time_write = 0
        last_pos_check = 0

        while True:
            # Check for commands
            if os.path.exists(CMD_FILE):
                try:
                    with open(CMD_FILE) as f:
                        cmd = json.load(f)
                    os.remove(CMD_FILE)
                    self.handle_cmd(cmd)
                except:
                    pass

            # Check for MIDI seek
            if os.path.exists(SEEK_FILE):
                try:
                    with open(SEEK_FILE) as f:
                        seek_data = json.load(f)
                    os.remove(SEEK_FILE)
                    position = seek_data.get('position', 0)
                    self.seek_to(position)
                except:
                    pass

            # Update time position from mpv
            now = time.time()
            if now - last_pos_check >= 0.5:
                pos = self._get_mpv_property('time-pos')
                if pos is not None:
                    self.time_pos = pos
                last_pos_check = now

            # Write state periodically
            if now - last_time_write >= 1.0:
                self.write_state()
                last_time_write = now

            # Handle end of file
            if self.process and self.process.poll() is not None:
                self.process = None
                if self.loop:
                    self.play_current()
                else:
                    self.next()

            time.sleep(0.03)  # ~30Hz poll for responsive seeking

if __name__ == '__main__':
    player = VideoPlayer()
    try:
        player.run()
    except KeyboardInterrupt:
        player.stop()
