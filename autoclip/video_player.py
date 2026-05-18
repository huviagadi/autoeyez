"""
video_player.py — MP4 Clip Playback Engine for autoclip
Part of the autoeyez video synthesis system

This module handles all clip playback functionality for the autoclip Pi 5.
It manages a directory of MP4 clips and provides frame-by-frame access to
the currently playing video for use by the mixer.

Architecture:
    /boot/firmware/clips/
        → ffmpeg subprocess (decode + scale)
        → raw RGB frames → numpy array
        → mixer.py calls get_frame()

    video_control.py
        → JSON commands → /tmp/player_cmd.json
        → THIS SCRIPT polls and executes
        → writes state → /tmp/player_state.json
        → video_control.py reads and sends to automidi

Playback features:
    - Play/pause with position memory
    - Loop current clip or auto-advance
    - Next/previous clip navigation
    - Jump to specific clip by index
    - Seek support via ffmpeg -ss

Frame pipeline:
    - ffmpeg decodes MP4 at realtime rate (-re flag)
    - Output scaled to 720x576 (PAL resolution)
    - Raw RGB24 piped to stdout
    - Background thread reads frames into numpy array
    - Thread-safe access via lock for mixer

IPC files:
    /tmp/player_cmd.json   - Commands from video_control.py (we read)
    /tmp/player_state.json - Current state for video_control.py (we write)
"""

import subprocess
import os
import time
import json
import numpy as np
import threading

# =============================================================================
# CONFIGURATION
# =============================================================================

# Clip storage location - on boot partition for easy access via card reader
CLIPS_DIR  = '/boot/firmware/clips/'

# IPC file paths for communication with video_control.py
STATE_FILE = '/tmp/player_state.json'  # We write our state here
CMD_FILE   = '/tmp/player_cmd.json'    # We read commands from here

# Output resolution (PAL standard for composite compatibility)
W, H = 720, 576

# =============================================================================
# UTILITY FUNCTIONS
# =============================================================================

def get_clips():
    """
    Get sorted list of MP4 files from clips directory.

    Clips are sorted alphabetically. Use numeric prefixes in filenames
    to control playback order (e.g., 01_intro.mp4, 02_main.mp4).

    Returns:
        List of MP4 filenames sorted alphabetically
    """
    return sorted([f for f in os.listdir(CLIPS_DIR) if f.endswith('.mp4')])

# =============================================================================
# VIDEO PLAYER CLASS
# =============================================================================

class VideoPlayer:
    """
    Manages MP4 clip playback via ffmpeg subprocess.

    Provides frame-by-frame access to decoded video for the mixer to
    composite with other sources. Handles playback control commands
    received via JSON IPC from video_control.py.

    Attributes:
        clips: List of available MP4 filenames
        index: Current clip index in the list
        paused: True if playback is paused
        pause_pos: Playback position (seconds) when paused, for resume
        loop: True if current clip should loop, False to auto-advance
        process: Active ffmpeg subprocess or None
        current_frame: Latest decoded frame as numpy array (H, W, 3)
        frame_lock: Threading lock for safe frame access
        time_pos: Current playback position in seconds
        duration: Total duration of current clip in seconds
        start_time: Wall-clock time when playback started (for position calc)
    """

    def __init__(self):
        """
        Initialize the video player.

        Loads clip list, sets up initial state, and prepares a blank
        frame buffer. Does not start playback - call run() for that.
        """
        self.clips = get_clips()
        self.index = 0
        self.paused = False
        self.pause_pos = 0        # Position to resume from after pause
        self.loop = False
        self.process = None

        # Frame buffer - initialized to black, updated by reader thread
        self.current_frame = np.zeros((H, W, 3), dtype=np.uint8)
        self.frame_lock = threading.Lock()

        # Playback timing
        self.time_pos = 0
        self.duration = 0
        self.start_time = None

        # Write initial state so video_control knows we're alive
        self.write_state()

    def write_state(self):
        """
        Write current player state to JSON file for video_control.py.

        State includes current clip info, playback position, and flags.
        Called whenever state changes (clip change, pause, loop toggle).
        video_control.py polls this file and sends updates to automidi.
        """
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
            pass  # Silently fail - state updates are best-effort

    def get_duration(self, clip_path):
        """
        Get duration of a video file using ffprobe.

        Args:
            clip_path: Full path to the MP4 file

        Returns:
            Duration in seconds as float, or 0 if probe fails
        """
        try:
            result = subprocess.run(
                ['ffprobe', '-v', 'error', '-show_entries', 'format=duration',
                 '-of', 'default=noprint_wrappers=1:nokey=1', clip_path],
                capture_output=True, text=True)
            return float(result.stdout.strip())
        except:
            return 0

    def play_current(self, seek=0):
        """
        Start playback of the current clip.

        Stops any existing playback, then launches ffmpeg to decode
        the current clip and pipe raw frames to stdout. A background
        thread reads these frames into the current_frame buffer.

        Args:
            seek: Starting position in seconds (default 0 = beginning)
        """
        # Stop any existing playback first
        self.stop()

        if not self.clips:
            return

        clip = os.path.join(CLIPS_DIR, self.clips[self.index])

        # Get clip duration for progress tracking
        self.duration = self.get_duration(clip)
        self.time_pos = seek
        self.start_time = time.time() - seek  # Adjust for seek offset

        # Build ffmpeg command
        # -re: Read input at native framerate (realtime playback)
        cmd = [
            'ffmpeg', '-re',
        ]

        # Add seek position if resuming
        if seek > 0:
            cmd += ['-ss', str(seek)]

        cmd += [
            '-i', clip,
            '-vf', 'scale={}:{}'.format(W, H),  # Scale to output resolution
            '-pix_fmt', 'rgb24',                 # Raw RGB, 3 bytes per pixel
            '-f', 'rawvideo', 'pipe:1'           # Output raw frames to stdout
        ]

        # Launch ffmpeg subprocess
        self.process = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL  # Suppress ffmpeg logging
        )

        # Start background thread to read frames
        threading.Thread(target=self._read_frames, daemon=True).start()
        self.write_state()

    def _read_frames(self):
        """
        Background thread: read raw frames from ffmpeg stdout.

        Runs continuously while ffmpeg is active, reading exactly
        one frame at a time (W * H * 3 bytes) and updating the
        current_frame buffer. Thread-safe via frame_lock.

        This method is launched as a daemon thread by play_current().
        """
        frame_size = W * H * 3  # RGB24 = 3 bytes per pixel
        proc = self.process     # Local reference in case process changes

        while proc and proc.poll() is None:
            try:
                # Read exactly one frame worth of data
                raw = proc.stdout.read(frame_size)

                if len(raw) == frame_size:
                    # Decode raw bytes to numpy array
                    frame = np.frombuffer(raw, dtype=np.uint8).reshape((H, W, 3))

                    # Thread-safe update of shared frame buffer
                    with self.frame_lock:
                        self.current_frame = frame.copy()
                elif len(raw) == 0:
                    # End of stream (clip finished)
                    break
            except:
                break

    def get_frame(self):
        """
        Get the current video frame (thread-safe).

        Called by mixer.py to get the latest decoded frame for
        compositing. Returns a copy to avoid race conditions.

        Returns:
            numpy array of shape (H, W, 3) with RGB pixel data
        """
        with self.frame_lock:
            return self.current_frame.copy()

    def stop(self):
        """
        Stop the current ffmpeg playback subprocess.

        Terminates gracefully with SIGTERM, then SIGKILL if needed.
        Called before starting a new clip or when pausing.
        """
        if self.process:
            self.process.terminate()
            try:
                self.process.wait(timeout=2)  # Wait for clean exit
            except:
                self.process.kill()  # Force kill if hung
            self.process = None

    def next(self):
        """
        Advance to the next clip in the list (wraps around).

        Resets pause position and starts playback from beginning.
        """
        self.index = (self.index + 1) % len(self.clips)
        self.pause_pos = 0
        self.play_current()

    def prev(self):
        """
        Go to the previous clip in the list (wraps around).

        Resets pause position and starts playback from beginning.
        """
        self.index = (self.index - 1) % len(self.clips)
        self.pause_pos = 0
        self.play_current()

    def play_index(self, i):
        """
        Jump to a specific clip by index.

        Args:
            i: Clip index (wraps around if out of range)
        """
        self.index = i % len(self.clips)
        self.play_current()
        self.pause_pos = 0

    def handle_cmd(self, cmd):
        """
        Execute a command received via IPC.

        Commands arrive as dictionaries with an 'action' key.
        This method dispatches to the appropriate handler.

        Args:
            cmd: Dictionary with 'action' and optional parameters
                 Actions: next, prev, play_index, pause, play,
                         loop_on, loop_off
        """
        action = cmd.get('action')

        if action == 'next':
            self.next()

        elif action == 'prev':
            self.prev()

        elif action == 'play_index':
            # Jump to specific clip - index in 'index' field
            self.play_index(cmd.get('index', 0))

        elif action == 'pause':
            # Pause playback, save position for resume
            self.paused = True
            self.pause_pos = self.time_pos
            self.stop()
            self.write_state()

        elif action == 'play':
            # Resume from paused position
            self.paused = False
            self.play_current(seek=self.pause_pos)

        elif action == 'loop_on':
            self.loop = True
            self.write_state()

        elif action == 'loop_off':
            self.loop = False
            self.write_state()

    def run(self):
        """
        Main run loop - start playback and process commands.

        Starts playing the first clip, then enters infinite loop:
        1. Poll for commands from video_control.py
        2. Handle end-of-clip (loop or advance)
        3. Update playback position for state file

        This method never returns - it's the main entry point.
        """
        self.play_current()
        last_time_write = 0  # Rate-limit state writes

        while True:
            # --- Check for incoming commands ---
            if os.path.exists(CMD_FILE):
                try:
                    with open(CMD_FILE) as f:
                        cmd = json.load(f)
                    os.remove(CMD_FILE)  # Consume the command
                    self.handle_cmd(cmd)
                except:
                    pass

            # --- Handle clip end (ffmpeg process exited) ---
            if not self.paused and self.process and self.process.poll() is not None:
                if self.loop:
                    # Restart same clip from beginning
                    self.play_current()
                else:
                    # Auto-advance to next clip
                    self.next()

            # --- Update playback position (~1/second) ---
            if not self.paused and self.start_time:
                now = time.time()
                if now - last_time_write >= 1.0:
                    # Calculate position from wall-clock time since start
                    self.time_pos = min(int(now - self.start_time), int(self.duration))
                    self.write_state()
                    last_time_write = now

            # Poll interval - 100ms is responsive enough for control
            time.sleep(0.1)

# =============================================================================
# ENTRY POINT
# =============================================================================

if __name__ == '__main__':
    player = VideoPlayer()
    player.run()
