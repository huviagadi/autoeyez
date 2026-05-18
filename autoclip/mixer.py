"""
mixer.py — Video Compositor for autoclip
Part of the autoeyez video synthesis system

This is the main process for the autoclip Pi 5. It captures video from
multiple sources, composites them together with crossfade and luma keying,
then outputs to both the local display and a network stream to autowaaave.

Architecture:
    Video Sources:
        - Clip player (video_player.py via get_frame())
        - 3× USB composite capture cards (/dev/video0, video2, video4)
        - Oscilloscope generator (audio visualization, currently disabled)

    Processing:
        - A/B crossfade between any two sources
        - Luma keying to overlay a third source based on brightness

    Output:
        - Local framebuffer (/dev/fb0) for HDMI preview
        - TCP stream to autowaaave (10.0.0.1:1236) for shader processing

IPC:
    /tmp/mixer_cmd.json   - Commands from video_control.py (we read)
    /tmp/mixer_state.json - Current state for video_control.py (we write)

Systemd:
    Runs as root via video.service (needed for /dev/fb0 access)
    Starts video_player.py as a background thread

USB Capture Cards:
    Using cheap USB composite-to-MJPEG capture dongles. Device numbers
    increment by 2 (/dev/video0, video2, video4) because each creates
    two nodes (one for capture, one for metadata).
"""

import subprocess
import numpy as np
import cv2
import threading
import time
import os
import json
import sys

# =============================================================================
# CONFIGURATION
# =============================================================================

# Output resolution (PAL standard for composite compatibility)
W, H = 720, 576

# Framebuffer device for local HDMI output
FBDEV = '/dev/fb0'

# IPC file paths for communication with video_control.py
STATE_FILE   = '/tmp/mixer_state.json'  # We write our state here
CMD_FILE     = '/tmp/mixer_cmd.json'    # We read commands from here

# =============================================================================
# SOURCE INDEX CONSTANTS
# =============================================================================

# These indices identify video sources for channel A, B, and luma key
SRC_CLIPS        = 0  # MP4 clip playback from video_player.py
SRC_COMPOSITE1   = 1  # First USB capture card
SRC_COMPOSITE2   = 2  # Second USB capture card
SRC_COMPOSITE3   = 3  # Third USB capture card
SRC_OSCILLOSCOPE = 4  # Audio oscilloscope visualization (disabled)

# =============================================================================
# CAPTURE DEVICE CONFIGURATION
# =============================================================================

# USB capture card device paths and formats
# These cheap dongles output MJPEG at 640x480 max resolution
# 'pad': True means we need to scale/letterbox to output resolution
CAPTURE_DEVICES = [
    {'device': '/dev/video0', 'fmt': 'MJPG', 'w': 640, 'h': 480, 'pad': True},
    {'device': '/dev/video2', 'fmt': 'MJPG', 'w': 640, 'h': 480, 'pad': True},
    {'device': '/dev/video4', 'fmt': 'MJPG', 'w': 640, 'h': 480, 'pad': True},
]

# =============================================================================
# CAPTURE CHANNEL CLASS
# =============================================================================

class CaptureChannel:
    """
    Captures video from a USB capture device via ffmpeg.

    Runs ffmpeg in a background thread to continuously decode frames
    from a V4L2 capture device. Frames are stored in a thread-safe
    buffer for access by the mixer.

    Includes automatic reconnection on capture failure and a watchdog
    system to detect stalled captures.

    Attributes:
        device: V4L2 device path (e.g., /dev/video0)
        fmt: Capture format ('MJPG' or 'YUYV')
        cw, ch: Capture resolution (input from device)
        pad: Whether to scale/pad to output resolution
        frame: Latest captured frame as numpy array
        lock: Threading lock for frame buffer access
        running: True while capture loop should run
        process: Current ffmpeg subprocess
        last_frame_time: Timestamp of last successful frame (for watchdog)
        first_frame: True once first frame received (for startup detection)
    """

    def __init__(self, device, fmt, w=720, h=576, pad=False):
        """
        Initialize a capture channel.

        Args:
            device: V4L2 device path
            fmt: Input format ('MJPG' or 'YUYV')
            w: Capture width
            h: Capture height
            pad: Scale to output resolution if True
        """
        self.device  = device
        self.fmt     = fmt
        self.cw      = w       # Capture width
        self.ch      = h       # Capture height
        self.pad     = pad

        # Frame buffer - initialized to black
        self.frame   = np.zeros((H, W, 3), dtype=np.uint8)
        self.lock    = threading.Lock()

        # Process state
        self.running = False
        self.process = None

        # Watchdog timing - start with future time to allow initial connect
        self.last_frame_time = time.time() + 10
        self.first_frame = False

    def start(self):
        """Start the capture thread."""
        self.running = True
        threading.Thread(target=self._capture, daemon=True).start()

    def _capture(self):
        """
        Background capture loop.

        Continuously runs ffmpeg to capture from the device. On failure,
        backs off and retries. Uses exponential backoff for fast failures
        to avoid hammering a disconnected device.
        """
        while self.running:
            try:
                start_time = time.time()

                # Map format names to ffmpeg input_format values
                fmt_map = {'MJPG': 'mjpeg', 'YUYV': 'yuyv422'}
                input_fmt = fmt_map.get(self.fmt)

                # YUYV is slower, use lower framerate
                fps = '15' if self.fmt == 'YUYV' else '30'

                # Build ffmpeg capture command
                cmd = ['ffmpeg', '-loglevel', 'error',
                       '-f', 'v4l2']

                if input_fmt:
                    cmd += ['-input_format', input_fmt]

                cmd += [
                    '-video_size', '{}x{}'.format(self.cw, self.ch),
                    '-framerate', fps,
                    '-i', self.device,
                ]

                # Scale to output resolution
                if self.pad:
                    vf = 'scale={}:{}'.format(W, H)
                else:
                    vf = 'scale={}:{}'.format(W, H)

                cmd += [
                    '-vf', vf,
                    '-pix_fmt', 'rgb24',     # Raw RGB output
                    '-f', 'rawvideo',
                    'pipe:1'                  # Output to stdout
                ]

                # Launch ffmpeg subprocess
                self.process = subprocess.Popen(
                    cmd,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.DEVNULL
                )

                frame_size = W * H * 3
                frame_count = 0

                # Read frames continuously
                while self.running:
                    raw = self.process.stdout.read(frame_size)

                    if len(raw) == frame_size:
                        # Decode and store frame
                        frame = np.frombuffer(raw, dtype=np.uint8).reshape((H, W, 3))
                        with self.lock:
                            self.frame = frame.copy()

                        frame_count += 1
                        self.last_frame_time = time.time()
                        self.first_frame = True

                        if frame_count == 1:
                            print("First frame received: {}".format(self.device), flush=True)

                    elif len(raw) == 0:
                        # Pipe closed - device disconnected or error
                        print("Pipe closed: {}".format(self.device), flush=True)
                        break
                    else:
                        # Partial frame - shouldn't happen normally
                        print("Short read {} bytes: {}".format(len(raw), self.device), flush=True)

            except Exception as e:
                print("Capture error {}: {}".format(self.device, e))

            finally:
                # Clean up subprocess
                elapsed = time.time() - start_time
                if self.process:
                    self.process.terminate()
                    self.process = None

                # Backoff logic: if we failed fast, wait longer before retry
                if elapsed < 3:
                    print("Fast fail on {} ({:.1f}s), backing off".format(
                        self.device, elapsed), flush=True)
                    time.sleep(15)  # Long wait for device that's not present
                else:
                    time.sleep(1)   # Short wait for transient errors

    def get_frame(self):
        """
        Get the current frame (thread-safe).

        Returns:
            numpy array of shape (H, W, 3) with RGB pixel data
        """
        with self.lock:
            return self.frame

# =============================================================================
# OSCILLOSCOPE CHANNEL CLASS
# =============================================================================

class OscilloscopeChannel:
    """
    Generates audio oscilloscope visualization.

    Captures audio from a sound device and renders an XY oscilloscope
    display (like a Lissajous pattern). Falls back to a synthetic
    pattern if audio capture fails.

    Currently disabled in the mixer (self.osc.start() commented out)
    but the code remains for future use.

    Attributes:
        frame: Current oscilloscope frame as numpy array
        lock: Threading lock for frame access
        running: True while generation loop should run
        t: Animation time counter for fallback pattern
    """

    def __init__(self):
        """Initialize the oscilloscope channel."""
        self.frame   = np.zeros((H, W, 3), dtype=np.uint8)
        self.lock    = threading.Lock()
        self.running = False
        self.t       = 0.0  # Animation time for fallback pattern

    def start(self):
        """Start the oscilloscope generation thread."""
        self.running = True
        threading.Thread(target=self._generate, daemon=True).start()

    def _generate(self):
        """
        Background generation loop.

        Attempts to capture audio and render as XY oscilloscope.
        Falls back to synthetic Lissajous pattern on error.
        """
        try:
            import sounddevice as sd

            # Audio capture buffer
            buf_size = 512
            buf = np.zeros((buf_size, 2), dtype=np.float32)
            buf_lock = threading.Lock()

            def audio_callback(indata, frames, time_info, status):
                """Audio stream callback - copy samples to buffer."""
                with buf_lock:
                    buf[:] = indata[:buf_size]

            # Start audio capture stream
            stream = sd.InputStream(
                device=1,            # Audio input device index
                samplerate=48000,
                channels=2,          # Stereo for XY display
                blocksize=buf_size,
                callback=audio_callback
            )
            stream.start()

            # Main generation loop
            while self.running:
                # Get audio data
                with buf_lock:
                    x_chan = buf[:, 0].copy()  # Left channel → X
                    y_chan = buf[:, 1].copy()  # Right channel → Y

                # Fade previous frame (persistence effect)
                with self.lock:
                    frame = (self.frame.astype(np.float32) * 0.4).astype(np.uint8)

                # Display parameters
                cx, cy = W // 2, H // 2
                scale_x = (W // 2 - 10)
                scale_y = (H // 2 - 10)

                # Normalize audio to -1..1 range
                x_norm = x_chan - np.mean(x_chan)
                y_norm = y_chan - np.mean(y_chan)

                x_max = np.max(np.abs(x_norm)) + 1e-6
                y_max = np.max(np.abs(y_norm)) + 1e-6
                x_norm = x_norm / x_max
                y_norm = y_norm / y_max

                # Phase shift Y for visual interest
                shift = len(x_norm) // 4
                y_shifted = np.roll(y_norm, shift)

                # Generate point list (subsample for performance)
                step = max(1, len(x_norm) // 256)
                pts = []
                for i in range(0, len(x_norm), step):
                    px = int(cx + x_norm[i] * scale_x)
                    py = int(cy - y_shifted[i] * scale_y)
                    px = np.clip(px, 0, W - 1)
                    py = np.clip(py, 0, H - 1)
                    pts.append((px, py))

                # Draw connected lines between points
                for i in range(1, len(pts)):
                    cv2.line(frame, pts[i-1], pts[i], (0, 255, 0), 1, cv2.LINE_AA)

                with self.lock:
                    self.frame = frame

                time.sleep(1.0 / 50.0)  # ~50 fps update rate

            stream.stop()

        except Exception as e:
            # Fallback: synthetic Lissajous pattern
            print("Oscilloscope error: {}".format(e))

            while self.running:
                frame = np.zeros((H, W, 3), dtype=np.uint8)
                cx, cy = W // 2, H // 2
                sx, sy = W // 2 - 10, H // 2 - 10

                self.t += 0.02  # Animation speed

                # Generate Lissajous curve points
                pts = []
                for i in range(512):
                    angle = i * np.pi * 2 / 512
                    px = int(cx + np.sin(angle * 3 + self.t) * sx)
                    py = int(cy + np.sin(angle * 2) * sy)
                    pts.append((np.clip(px, 0, W-1), np.clip(py, 0, H-1)))

                # Draw the curve
                for i in range(1, len(pts)):
                    cv2.line(frame, pts[i-1], pts[i], (0, 255, 0), 1, cv2.LINE_AA)

                with self.lock:
                    self.frame = frame

                time.sleep(1.0 / 15.0)  # Lower fps for fallback

    def get_frame(self):
        """
        Get the current oscilloscope frame (thread-safe).

        Returns:
            numpy array of shape (H, W, 3) with RGB pixel data
        """
        with self.lock:
            return self.frame.copy()

# =============================================================================
# MIXER CLASS
# =============================================================================

class Mixer:
    """
    Main video compositor and output manager.

    Combines multiple video sources using crossfade and luma keying,
    then outputs to both local framebuffer and network stream.

    Compositing features:
        - Channel A/B selection from any source
        - Crossfade between A and B (0-127)
        - Luma key overlay from any source with threshold control
        - High threshold option for band keying

    Output:
        - Framebuffer at 30fps (BGRA format for /dev/fb0)
        - TCP stream to autowaaave (H.264 via ffmpeg)

    Attributes:
        stream_process: ffmpeg process for network streaming
        clip_player: Reference to VideoPlayer instance for clip frames
        captures: List of CaptureChannel instances
        dummy: Black frame for fallback
        osc: OscilloscopeChannel instance
        ch_a, ch_b: Source indices for channels A and B
        mix: Crossfade value (0=full A, 127=full B)
        luma: Low threshold for luma key (0-127)
        luma_src: Source for luma key (0=none, 1-4=source index)
        luma_high: High threshold for band keying
        luma_high_enabled: True to enable high threshold cutoff
        fb: File handle for framebuffer device
        out_buf, blend_out, luma_out: Pre-allocated buffers for speed
    """

    def __init__(self, clip_player):
        """
        Initialize the mixer.

        Args:
            clip_player: VideoPlayer instance to get clip frames from
        """
        # Start network stream first (runs in background thread)
        self.stream_process = None
        self._start_stream()

        # Reference to clip player for frame access
        self.clip_player = clip_player

        # Initialize capture channels from config
        self.captures = [CaptureChannel(**d) for d in CAPTURE_DEVICES]

        # Fallback frame (black)
        self.dummy    = np.zeros((H, W, 3), dtype=np.uint8)

        # Oscilloscope (currently unused)
        self.osc      = OscilloscopeChannel()

        # Mixer state - default to clips on A, first composite on B
        self.ch_a      = SRC_CLIPS
        self.ch_b      = SRC_COMPOSITE1
        self.mix       = 0      # Full channel A

        # Luma key settings
        self.luma      = 0      # Low threshold (0=off)
        self.luma_src  = 0      # Source (0=none)
        self.luma_high         = 127   # High threshold
        self.luma_high_enabled = False # High cut disabled

        # Pre-allocated output buffers (avoids per-frame allocation)
        self.luma_out  = np.empty((H, W, 3), dtype=np.uint8)
        self.blend_out = np.empty((H, W, 3), dtype=np.uint8)

        # Open framebuffer for direct output
        self.fb = open(FBDEV, 'wb')
        self.out_buf = np.empty((H, W, 4), dtype=np.uint8)  # BGRA format

        self.write_state()

    def write_state(self):
        """
        Write current mixer state to JSON file for video_control.py.

        Currently not actively read by anything, but available for
        future state sync features.
        """
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
        """
        Get frame from a source by index.

        Args:
            src: Source index (SRC_CLIPS, SRC_COMPOSITE1, etc.)

        Returns:
            numpy array of shape (H, W, 3) with RGB pixel data
        """
        if src == SRC_CLIPS:
            return self.clip_player.get_frame()
        elif src == SRC_COMPOSITE1:
            return self.captures[0].get_frame()
        elif src == SRC_COMPOSITE2:
            return self.captures[1].get_frame()
        elif src == SRC_COMPOSITE3:
            return self.captures[2].get_frame()
        elif src == SRC_OSCILLOSCOPE:
            return self.dummy.copy()  # Oscilloscope disabled
        return self.dummy.copy()

    def handle_cmd(self, cmd):
        """
        Execute a command received via IPC.

        Commands arrive as dictionaries with an 'action' key.
        This method updates mixer state accordingly.

        Args:
            cmd: Dictionary with 'action' and 'value' fields
                 Actions: ch_a, ch_b, mix, luma, luma_src,
                         luma_high, luma_high_en
        """
        action = cmd.get('action')

        if action == 'ch_a':
            # Set channel A source
            self.ch_a = cmd.get('value', 0)

        elif action == 'ch_b':
            # Set channel B source
            self.ch_b = cmd.get('value', 1)

        elif action == 'mix':
            # Set crossfade value (0-127)
            self.mix = cmd.get('value', 0)

        elif action == 'luma':
            # Set luma key low threshold
            # Constrain to not exceed high threshold
            self.luma = min(cmd.get('value', 0), self.luma_high)
            self.write_state()

        elif action == 'luma_src':
            # Set luma key source
            self.luma_src = cmd.get('value', 0)

        elif action == 'luma_high':
            # Set luma key high threshold
            # Constrain to not go below low threshold
            val = cmd.get('value', 127)
            self.luma_high = max(val, self.luma)
            self.write_state()

        elif action == 'luma_high_en':
            # Enable/disable high threshold cut
            self.luma_high_enabled = cmd.get('value', False)
            self.write_state()

        self.write_state()

    def composite(self):
        """
        Perform frame compositing.

        Combines channel A and B with crossfade, then optionally
        applies luma key overlay from a third source.

        Returns:
            numpy array of shape (H, W, 3) - composited RGB frame
        """
        # Optimization: only fetch frames we need
        need_a = True
        need_b = self.mix > 0 or self.luma > 0

        frame_a = self.get_source_frame(self.ch_a) if need_a else self.dummy
        frame_b = self.get_source_frame(self.ch_b) if need_b else self.dummy

        # --- Fast paths for pure A or B ---
        if self.mix == 0 and self.luma == 0:
            return frame_a

        if self.mix == 127 and self.luma == 0:
            return frame_b

        # --- Crossfade blend ---
        if self.mix > 0:
            mix_f = self.mix / 127.0  # Normalize to 0-1
            # Use pre-allocated buffer for output
            cv2.addWeighted(frame_a, 1.0 - mix_f, frame_b, mix_f, 0, dst=self.blend_out)
            out = self.blend_out
        else:
            out = frame_a

        # --- Luma key overlay ---
        if self.luma > 0 and self.luma_src > 0:
            # Get key source frame (source indices are 1-based, adjust to 0-based)
            key_frame = self.get_source_frame(self.luma_src - 1)

            # Convert to grayscale for threshold
            gray = cv2.cvtColor(key_frame, cv2.COLOR_RGB2GRAY)

            # Scale thresholds from 0-127 to 0-254 range
            luma_low_u8  = int(self.luma * 2)
            luma_high_u8 = int(self.luma_high * 2)

            if self.luma_high_enabled:
                # Band key: show key_frame only where brightness is between thresholds
                _, mask_low  = cv2.threshold(gray, luma_low_u8,  255, cv2.THRESH_BINARY)
                _, mask_high = cv2.threshold(gray, luma_high_u8, 255, cv2.THRESH_BINARY)
                mask = cv2.bitwise_and(mask_low, cv2.bitwise_not(mask_high))
            else:
                # Low key only: show key_frame where brightness exceeds threshold
                _, mask = cv2.threshold(gray, luma_low_u8, 255, cv2.THRESH_BINARY)

            # Apply mask - where mask is white, use key_frame; else use blended output
            out = np.where(mask[:,:,np.newaxis] > 0, key_frame, out)

        return out

    def write_frame(self, frame):
        """
        Output a frame to framebuffer and network stream.

        Converts RGB to BGRA for framebuffer, sends RGB to stream.

        Args:
            frame: numpy array of shape (H, W, 3) with RGB pixel data
        """
        # Convert RGB to BGRA for framebuffer
        self.out_buf[:,:,0] = frame[:,:,2]  # B
        self.out_buf[:,:,1] = frame[:,:,1]  # G
        self.out_buf[:,:,2] = frame[:,:,0]  # R
        self.out_buf[:,:,3] = 255           # A (opaque)

        # Write to framebuffer
        self.fb.write(self.out_buf.tobytes())
        self.fb.seek(0)  # Seek back to start for next frame

        # Send to network stream (if connected)
        if self.stream_process and self.stream_process.poll() is None:
            try:
                self.stream_process.stdin.write(frame.tobytes())
            except:
                pass  # Stream may have disconnected

    def start(self):
        """
        Start all capture channels and background threads.

        Waits for each capture to confirm first frame before
        starting the next (helps avoid USB bandwidth issues).
        """
        time.sleep(5)  # Wait for system to settle after boot

        # Start capture channels one at a time
        for i, ch in enumerate(self.captures):
            ch.start()

            # Wait for first frame or timeout
            deadline = time.time() + 10
            while not ch.first_frame and time.time() < deadline:
                time.sleep(0.5)

            if ch.first_frame:
                print("Capture confirmed: {}".format(ch.device), flush=True)
            else:
                print("Capture timeout: {}".format(ch.device), flush=True)

            time.sleep(1)  # Brief pause between starting channels

        # Oscilloscope disabled for now
        # self.osc.start()

        # Start background threads
        threading.Thread(target=self._watchdog, daemon=True).start()
        threading.Thread(target=self._cmd_thread, daemon=True).start()

    def _watchdog(self):
        """
        Background thread: monitor capture channels for stalls.

        Checks each channel's last_frame_time and restarts ffmpeg
        if frames stop arriving. Uses different timeouts for initial
        connection vs. established streams.
        """
        while True:
            time.sleep(5)

            for ch in self.captures:
                age = time.time() - ch.last_frame_time

                # Use shorter timeout before first frame (device may not be present)
                threshold = 8 if not ch.first_frame else 15

                if age > threshold:
                    print("Watchdog restarting capture: {} (first_frame={})".format(
                        ch.device, ch.first_frame), flush=True)

                    # Reset state and kill process
                    ch.first_frame = False
                    if ch.process:
                        ch.process.terminate()
                        ch.process = None

                    # Push deadline forward to allow restart
                    ch.last_frame_time = time.time() + 20

    def _cmd_thread(self):
        """
        Background thread: poll for IPC commands.

        Checks for command file at high frequency (100Hz) for
        responsive control from automidi.
        """
        while True:
            if os.path.exists(CMD_FILE):
                try:
                    with open(CMD_FILE) as f:
                        cmd = json.load(f)
                    os.remove(CMD_FILE)  # Consume the command
                    self.handle_cmd(cmd)
                except:
                    pass
            time.sleep(0.01)  # 100Hz poll rate

    def run(self):
        """
        Main run loop - start captures and composite at 30fps.

        This method never returns - it's the main entry point.
        """
        self.start()

        target_fps = 30
        frame_time = 1.0 / target_fps

        while True:
            t0 = time.time()

            # Composite and output frame
            frame = self.composite()
            self.write_frame(frame)

            # Maintain target framerate
            elapsed = time.time() - t0
            sleep_t = frame_time - elapsed

            if sleep_t > 0:
                time.sleep(sleep_t)
            else:
                # Log overruns for debugging
                print("Frame overrun: {:.1f}ms".format(elapsed * 1000), flush=True)

    def _start_stream(self):
        """
        Start the network stream to autowaaave.

        Launches ffmpeg in a background thread to encode frames
        as H.264 and send via TCP to autowaaave (10.0.0.1:1236).

        The thread automatically reconnects if the connection drops.
        """
        def _connect():
            while True:
                # Build ffmpeg streaming command
                cmd = [
                    'ffmpeg', '-loglevel', 'quiet',
                    '-f', 'rawvideo', '-pix_fmt', 'rgb24',
                    '-video_size', '{}x{}'.format(W, H),
                    '-framerate', '30',
                    '-i', 'pipe:0',                     # Input from stdin
                    '-vf', 'scale=720:480',             # Scale to NTSC resolution
                    '-pix_fmt', 'yuv420p',
                    '-c:v', 'libx264',
                    '-preset', 'ultrafast',             # Lowest latency
                    '-tune', 'zerolatency',
                    '-crf', '18',                       # Good quality
                    '-f', 'mpegts',
                    'tcp://10.0.0.1:1236'               # autowaaave address
                ]

                self.stream_process = subprocess.Popen(
                    cmd, stdin=subprocess.PIPE,
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
                )

                # Block until stream process exits
                self.stream_process.wait()

                print("Stream disconnected, retrying...", flush=True)
                time.sleep(2)  # Wait before reconnect

        threading.Thread(target=_connect, daemon=True).start()
        print("Stream started", flush=True)

# =============================================================================
# ENTRY POINT
# =============================================================================

if __name__ == '__main__':
    # Add home directory to path for local imports
    sys.path.insert(0, '/home/admin')
    from video_player import VideoPlayer

    # Start clip player in background thread
    player = VideoPlayer()
    threading.Thread(target=player.run, daemon=True).start()

    # Wait for player to initialize
    time.sleep(2)

    # Start mixer (blocks forever)
    mixer = Mixer(player)
    mixer.run()
