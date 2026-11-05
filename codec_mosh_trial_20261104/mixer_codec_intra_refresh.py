from patch_transport import FrameInbox
"""
mixer.py — Video Compositor for autoclip
Part of the autoeyez video synthesis system

This is the main process for the autoclip Pi 5. It captures video from
multiple sources, composites them together with crossfade and luma keying,
then outputs to both the local display and a network stream to autowaaave.

Architecture:
    Video Sources:
        - Clip player (video_player.py via get_frame())
        - 3× USB composite capture cards (stable USB-port paths)
        - External USB capture (hot-swappable, stable USB-port path)

    Processing:
        - A/B crossfade between any two sources
        - Luma keying to overlay a third source based on brightness

    Output:
        - Local framebuffer (/dev/fb0) for HDMI preview
        - TCP stream to autowaaave (raw YUV420p on port 1237) for shader processing

IPC:
    /tmp/autoeyez_mixer_commands.sock - Acknowledged local controls
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
import math
import os
import json
import sys
import socket
import struct
import select
import glob
import gpu_profile
import trace_runtime
from command_ipc import CommandReceiver, MIXER_COMMAND_SOCKET
from output_stutter import OutputStutter
from shader_beat_repeat import ShaderBeatRepeat
from shader_controls import default_patch, effective_values, validate_patch
from force_offset_map import bounded, MIXER_CC

from math_sources import SquircleSource
try:
    from gpu_shader_sources import (GpuShaderSource, EyeOfEzbhanGpuSource,
                                    VoxelGpuSource, PolyMangleGpuSource,
                                    AlienGpuSource, AncientGpuSource,
                                    CircuitGpuSource, HypnoGpuSource,
                                    MandelbrodtGpuSource, MoireGpuSource,
                                    QuasiGpuSource, ShatiGpuSource,
                                    SlicesGpuSource, SmpteGpuSource,
                                    SquircleGpuSource)
except ImportError:
    GpuShaderSource = None
    EyeOfEzbhanGpuSource = None
    VoxelGpuSource = None
    PolyMangleGpuSource = None
    AlienGpuSource = None
    AncientGpuSource = None
    CircuitGpuSource = None
    HypnoGpuSource = None
    MandelbrodtGpuSource = None
    MoireGpuSource = None
    QuasiGpuSource = None
    ShatiGpuSource = None
    SlicesGpuSource = None
    SmpteGpuSource = None
    SquircleGpuSource = None
from shader_catalog import NEW_SHADER_SPECS, shader_source

# =============================================================================
# CONFIGURATION
# =============================================================================

# Output resolution (PAL standard for composite compatibility)
W, H = 720, 576

# Framebuffer device for local HDMI output
FBDEV = '/dev/fb0'

# IPC file paths for communication with video_control.py
STATE_FILE   = '/tmp/mixer_state.json'  # We write our state here

# Raw YUV420p is the v04 wire format. H.264 remains selectable for rollback.
STREAM_FORMAT = os.environ.get('AUTOEYEZ_STREAM_FORMAT', 'yuv420p').lower()
if STREAM_FORMAT not in ('yuv420p', 'h264', 'h264_annexb'):
    raise ValueError('AUTOEYEZ_STREAM_FORMAT must be yuv420p, h264, or h264_annexb')
STREAM_HOST = os.environ.get('AUTOEYEZ_STREAM_HOST', '10.0.0.1')
STREAM_PORT = int(os.environ.get('AUTOEYEZ_STREAM_PORT',
                                 '1237' if STREAM_FORMAT == 'yuv420p' else '1236'))

# =============================================================================
# SOURCE INDEX CONSTANTS
# =============================================================================

# These indices identify video sources for channel A, B, and luma key
from source_catalog import (
    SRC_CLIPS,
    SRC_COMPOSITE1,
    SRC_COMPOSITE2,
    SRC_COMPOSITE3,
    SRC_EXT_USB,
    SRC_SQUIRCLE,
    SRC_MOIRE,
    SRC_QUASI,
    SRC_HYPNO,
    SRC_CIRCUIT,
    SRC_SMPTE,
    SRC_ANCIENT,
    SRC_SHATI,
    SRC_ALIEN,
    SRC_SLICES,
    SRC_MANDELBRODT
)

# =============================================================================
# CAPTURE DEVICE CONFIGURATION
# =============================================================================

# USB capture card device paths and formats. Use by-path aliases because V4L2
# node numbers can change after a hot-plug or uvcvideo driver reload.
# Composite capture cards: 720x480 @ 25fps max
# EXT USB: 640x480 @ 30fps
# 'fps': optional framerate override (default 30 for MJPG, 15 for YUYV)
CAPTURE_DEVICES = [
    {'device': '/dev/v4l/by-path/platform-xhci-hcd.0-usb-0:1:1.0-video-index0', 'fmt': 'MJPG', 'w': 720, 'h': 480, 'fps': 25, 'pad': True},
    {'device': '/dev/v4l/by-path/platform-xhci-hcd.1-usb-0:1:1.0-video-index0', 'fmt': 'MJPG', 'w': 720, 'h': 480, 'fps': 25, 'pad': True},
    {'device': '/dev/v4l/by-path/platform-xhci-hcd.1-usb-0:2:1.0-video-index0', 'fmt': 'MJPG', 'w': 720, 'h': 480, 'fps': 25, 'pad': True},
    {'device': None, 'fmt': 'MJPG', 'w': 640, 'h': 480, 'fps': 30, 'pad': True},  # EXT USB: discover by camera identity
]

def resolve_ext_usb_device():
    """Find a webcam regardless of which USB hub port it occupies."""
    composite_nodes = {os.path.realpath(entry['device'])
                       for entry in CAPTURE_DEVICES[:3]}
    candidates = [path for path in sorted(glob.glob('/dev/v4l/by-id/*-video-index0'))
                  if os.path.realpath(path) not in composite_nodes]
    preferred = os.environ.get('AUTOEYEZ_EXT_USB_ID', '')
    if preferred:
        candidates = [path for path in candidates if preferred in os.path.basename(path)]
    return candidates[0] if candidates else None

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

    def __init__(self, device, fmt, w=720, h=576, fps=30, pad=False):
        """
        Initialize a capture channel.

        Args:
            device: V4L2 device path
            fmt: Input format ('MJPG' or 'YUYV')
            w: Capture width
            h: Capture height
            fps: Target framerate (use device's actual capability)
            pad: Scale to output resolution if True
        """
        self.device  = device
        self.dynamic_device = device is None
        self.fmt     = fmt
        self.cw      = w       # Capture width
        self.ch      = h       # Capture height
        self.fps     = fps     # Target framerate
        self.pad     = pad

        # Frame buffer - initialized to black
        self.frame   = np.zeros((H, W, 3), dtype=np.uint8)
        self.lock    = threading.Lock()
        self.trace_source = trace_runtime.SourceTracker(device or 'EXT USB', fps) if trace_runtime.ENABLED else None

        # Process state
        self.running = False
        self.active  = False   # Only capture when active (source is in use)
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
            # Wait until this channel is needed (outside try/finally)
            if not self.active:
                # Stop any running capture when inactive
                if self.process:
                    self.process.kill()
                    try:
                        self.process.wait(timeout=1)
                    except:
                        pass
                    self.process = None
                self.last_frame_time = time.time()  # Prevent watchdog triggers
                time.sleep(0.5)
                continue

            start_time = time.time()
            try:
                capture_device = (resolve_ext_usb_device() if self.dynamic_device
                                  else self.device)
                # Check if device exists before trying to capture
                if not capture_device or not os.path.exists(capture_device):
                    print("Device not present: {}".format(self.device or 'EXT USB'), flush=True)
                    # Clear to black so user knows feed is down
                    with self.lock:
                        self.frame = np.zeros((H, W, 3), dtype=np.uint8)
                    time.sleep(5)  # Check again in 5 seconds
                    continue

                # Map format names to ffmpeg input_format values
                fmt_map = {'MJPG': 'mjpeg', 'YUYV': 'yuyv422'}
                input_fmt = fmt_map.get(self.fmt)

                # Use device-specific framerate
                fps = str(self.fps)

                # Build ffmpeg capture command with low-latency flags
                cmd = ['ffmpeg', '-loglevel', 'error',
                       '-fflags', 'nobuffer',        # No input buffering
                       '-flags', 'low_delay',        # Low latency decode
                       '-probesize', '32',           # Minimal probe
                       '-analyzeduration', '0',      # Skip analysis
                       '-f', 'v4l2']

                if input_fmt:
                    cmd += ['-input_format', input_fmt]

                cmd += [
                    '-video_size', '{}x{}'.format(self.cw, self.ch),
                    '-framerate', fps,
                    '-i', capture_device,
                ]

                # Scale to output resolution with fast algorithm
                vf = 'scale={}:{}:flags=fast_bilinear'.format(W, H)

                cmd += [
                    '-vf', vf,
                    '-pix_fmt', 'rgb24',     # Raw RGB output
                    '-f', 'rawvideo',
                    '-flush_packets', '1',   # Flush immediately
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
                pending = bytearray()
                stdout_fd = self.process.stdout.fileno()
                os.set_blocking(stdout_fd, False)

                # Read without blocking forever so deactivation can promptly
                # release the V4L2 device even when a camera has stalled.
                while self.running and self.active:
                    if self.process.poll() is not None:
                        print("Pipe closed: {}".format(capture_device), flush=True)
                        break

                    readable, _, _ = select.select([stdout_fd], [], [], 0.25)
                    if not readable:
                        continue

                    try:
                        chunk = os.read(stdout_fd, 262144)
                    except BlockingIOError:
                        continue

                    if not chunk:
                        print("Pipe closed: {}".format(capture_device), flush=True)
                        break

                    pending.extend(chunk)

                    while len(pending) >= frame_size:
                        raw = bytes(pending[:frame_size])
                        del pending[:frame_size]

                        # Decode and store frame
                        frame = np.frombuffer(raw, dtype=np.uint8).reshape((H, W, 3))
                        with self.lock:
                            self.frame = frame.copy()
                            if self.trace_source: self.trace_source.publish(self.frame)

                        frame_count += 1
                        self.last_frame_time = time.time()
                        self.first_frame = True

                        if frame_count == 1:
                            print("First frame received: {}".format(capture_device), flush=True)

            except Exception as e:
                print("Capture error {}: {}".format(self.device or 'EXT USB', e))

            finally:
                # Clean up subprocess - use kill() for faster cleanup
                elapsed = time.time() - start_time
                if self.process:
                    self.process.kill()  # SIGKILL - don't wait for graceful shutdown
                    try:
                        self.process.wait(timeout=2)  # Reap zombie process
                    except:
                        pass
                    self.process = None

                # Clear frame to black so user knows feed is down
                with self.lock:
                    self.frame = np.zeros((H, W, 3), dtype=np.uint8)

                # Backoff logic: if we failed fast, wait longer before retry
                if elapsed < 3:
                    print("Fast fail on {} ({:.1f}s), backing off".format(
                        self.device or 'EXT USB', elapsed), flush=True)
                    time.sleep(5)  # Wait for device to stabilize
                else:
                    time.sleep(1)  # Short wait for transient errors

    def get_trace_frame(self):
        with self.lock:
            return self.frame.copy(), self.trace_source.origin(self.frame,'live')

    def get_frame(self):
        """
        Get the current frame (thread-safe).

        Returns:
            numpy array of shape (H, W, 3) with RGB pixel data
        """
        with self.lock:
            return self.frame

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
        - TCP stream to autowaaave (raw YUV420p via ffmpeg)

    Attributes:
        stream_writer: bounded worker owning ffmpeg and network streaming
        clip_player: Reference to VideoPlayer instance for clip frames
        captures: List of CaptureChannel instances (including EXT USB)
        dummy: Black frame for fallback
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
        self.trace_selection = trace_runtime.Selection() if trace_runtime.ENABLED else None
        # Start TCP stream to autowaaave.
        self._start_stream()

        # Reference to clip player for frame access
        self.clip_player = clip_player
        self.output_stutter = OutputStutter()
        self.clip_stutter = OutputStutter()
        self.shader_beat_repeat = ShaderBeatRepeat()
        self.shader_clock_origin = time.monotonic()
        self.shader_clock_last = None
        self.shader_phase = [0.0, 0.0, 0.0]
        self.shader_role_clones = {}
        self.shader_patch = default_patch()
        self.shader_bands = (0.0, 0.0, 0.0)
        self.shader_fft_at = 0.0
        self._shader_active = False
        self._clip_stutter_frame = None
        self._clip_event_for_mix = None
        self._last_clip_event_submitted = None

        # Initialize capture channels from config
        self.captures = [CaptureChannel(**d) for d in CAPTURE_DEVICES]
        try:
            if SquircleGpuSource is None:
                raise RuntimeError('ModernGL shader runtime unavailable')
            self.squircle = SquircleGpuSource(
                1280, 720, output_width=W, output_height=H)
            print('GPU shader source ready: SQUIRCLE 1280x720 -> {}x{} ({})'.format(
                W, H, self.squircle.ctx.info.get('GL_RENDERER', 'unknown')),
                flush=True)
        except Exception as error:
            print('GPU shader fallback: {}'.format(error), flush=True)
            self.squircle = SquircleSource(W, H)
        self.moire = None
        try:
            if MoireGpuSource is None:
                raise RuntimeError('ModernGL shader runtime unavailable')
            self.moire = MoireGpuSource(
                1280, 720, output_width=W, output_height=H)
            print('GPU shader source ready: MOIRE 1280x720 -> {}x{} ({})'.format(
                W, H, self.moire.ctx.info.get('GL_RENDERER', 'unknown')),
                flush=True)
        except Exception as error:
            print('GPU shader unavailable: MOIRE: {}'.format(error), flush=True)
        self.quasi = None
        try:
            if QuasiGpuSource is None:
                raise RuntimeError('ModernGL shader runtime unavailable')
            self.quasi = QuasiGpuSource(
                1280, 720, output_width=W, output_height=H)
            print('GPU shader source ready: QUASI 1280x720 -> {}x{} ({})'.format(
                W, H, self.quasi.ctx.info.get('GL_RENDERER', 'unknown')),
                flush=True)
        except Exception as error:
            print('GPU shader unavailable: QUASI: {}'.format(error), flush=True)
        self.hypno = None
        try:
            if HypnoGpuSource is None:
                raise RuntimeError('ModernGL shader runtime unavailable')
            self.hypno = HypnoGpuSource(
                1280, 720, output_width=W, output_height=H)
            print('GPU shader source ready: HYPNO 1280x720 -> {}x{} ({})'.format(
                W, H, self.hypno.ctx.info.get('GL_RENDERER', 'unknown')),
                flush=True)
        except Exception as error:
            print('GPU shader unavailable: HYPNO: {}'.format(error), flush=True)
        self.circuit = None
        try:
            if CircuitGpuSource is None:
                raise RuntimeError('ModernGL shader runtime unavailable')
            self.circuit = CircuitGpuSource(
                1280, 720, output_width=W, output_height=H)
            print('GPU shader source ready: CIRCUIT 1280x720 -> {}x{} ({})'.format(
                W, H, self.circuit.ctx.info.get('GL_RENDERER', 'unknown')),
                flush=True)
        except Exception as error:
            print('GPU shader unavailable: CIRCUIT: {}'.format(error), flush=True)
        self.smpte = None
        try:
            if SmpteGpuSource is None:
                raise RuntimeError('ModernGL shader runtime unavailable')
            self.smpte = SmpteGpuSource(
                1280, 720, output_width=W, output_height=H)
            print('GPU shader source ready: SMPTE 1280x720 -> {}x{} ({})'.format(
                W, H, self.smpte.ctx.info.get('GL_RENDERER', 'unknown')),
                flush=True)
        except Exception as error:
            print('GPU shader unavailable: SMPTE: {}'.format(error), flush=True)
        self.ancient = None
        try:
            self.ancient = AncientGpuSource(
                1280, 720, output_width=W, output_height=H)
            print('GPU shader source ready: ANCIENT 1280x720 -> {}x{} ({})'.format(
                W, H, self.ancient.ctx.info.get('GL_RENDERER', 'unknown')), flush=True)
        except Exception as error:
            print('GPU shader unavailable: ANCIENT: {}'.format(error), flush=True)
        self.shati = None
        try:
            self.shati = ShatiGpuSource(
                1280, 720, output_width=W, output_height=H)
            print('GPU shader source ready: SHATI 1280x720 -> {}x{} ({})'.format(
                W, H, self.shati.ctx.info.get('GL_RENDERER', 'unknown')), flush=True)
        except Exception as error:
            print('GPU shader unavailable: SHATI: {}'.format(error), flush=True)
        self.alien = None
        try:
            self.alien = AlienGpuSource(
                1280, 720, output_width=W, output_height=H)
            print('GPU shader source ready: ALIEN 1280x720 -> {}x{} ({})'.format(
                W, H, self.alien.ctx.info.get('GL_RENDERER', 'unknown')), flush=True)
        except Exception as error:
            print('GPU shader unavailable: ALIEN: {}'.format(error), flush=True)
        self.slices = None
        try:
            self.slices = SlicesGpuSource(
                1280, 720, output_width=W, output_height=H)
            print('GPU shader source ready: SLICES 1280x720 -> {}x{} ({})'.format(
                W, H, self.slices.ctx.info.get('GL_RENDERER', 'unknown')), flush=True)
        except Exception as error:
            print('GPU shader unavailable: SLICES: {}'.format(error), flush=True)
        self.mandelbrodt = None
        try:
            # The 100-step fractal loop misses 30 fps at 1280x720. Rendering
            # at 854x480 keeps its p95 below the frame budget before the
            # standard GPU scale into the 720x576 mixer surface.
            self.mandelbrodt = MandelbrodtGpuSource(
                854, 480, output_width=W, output_height=H)
            print('GPU shader source ready: MANDELBRODT 854x480 -> {}x{} ({})'.format(
                W, H, self.mandelbrodt.ctx.info.get('GL_RENDERER', 'unknown')), flush=True)
        except Exception as error:
            print('GPU shader unavailable: MANDELBRODT: {}'.format(error), flush=True)

        self.extra_shaders = {}
        for index, name, filename in NEW_SHADER_SPECS:
            try:
                if GpuShaderSource is None:
                    raise RuntimeError('ModernGL shader runtime unavailable')
                if name == 'EYE OF EZBHAN':
                    shader = EyeOfEzbhanGpuSource(
                        shader_source(filename),
                        shader_source('eye_of_ezbhan_field.glsl'),
                        1280, 720, output_width=W, output_height=H)
                elif name == 'VOXELSCAPE':
                    shader = VoxelGpuSource(
                        shader_source(filename), 320, 180,
                        output_width=W, output_height=H)
                elif name == 'POLY MANGLE':
                    shader = PolyMangleGpuSource(
                        shader_source('poly_mangle.vert'),
                        shader_source(filename), 1280, 720,
                        output_width=W, output_height=H)
                elif name in ('RADIAL BANDS', 'SLOW SCOPE'):
                    # Draw at mixer aspect ratio so the radial meter stays circular.
                    shader = GpuShaderSource(
                        name, shader_source(filename), W, H,
                        output_width=W, output_height=H)
                else:
                    shader = GpuShaderSource(
                        name, shader_source(filename), 1280, 720,
                        output_width=W, output_height=H)
                self.extra_shaders[index] = shader
                print('GPU shader source ready: {} {}x{} -> {}x{} ({})'.format(
                    name, shader.width, shader.height, W, H,
                    shader.ctx.info.get('GL_RENDERER', 'unknown')),
                    flush=True)
            except Exception as error:
                print('GPU shader unavailable: {}: {}'.format(name, error), flush=True)

        # Fallback frame (black)
        self.dummy    = np.zeros((H, W, 3), dtype=np.uint8)

        # Mixer state - default to clips on A, first composite on B
        self.ch_a      = SRC_CLIPS
        self.ch_b      = SRC_COMPOSITE1
        self.mix       = 0      # Full channel A

        # Luma key settings
        self.luma      = 0      # Low threshold (0=off)
        self.luma_feather = 1      # Gaussian mask feather radius, 0..4 mixer pixels
        self.luma_src  = 0      # Source (0=none)
        self.luma_high         = 127   # High threshold
        self.luma_high_enabled = False # High cut disabled
        self.force_cc = {cc: 63 for cc in MIXER_CC}

        # Datamosh settings
        self.mosh_k       = 1.0    # Residual gate: 1.0=bypass, 0.02=full mosh
        self.mosh_enabled = False  # Quick toggle
        self.mosh_accum   = None   # Accumulator buffer (lazy init on first frame)
        self.mosh_prev    = None   # Previous frame for motion estimation
        self.mosh_mv      = None   # Motion vector field

        # Pre-allocated output buffers (avoids per-frame allocation)
        self.luma_out  = np.empty((H, W, 3), dtype=np.uint8)
        self.blend_out = np.empty((H, W, 3), dtype=np.uint8)

        # Open framebuffer for direct output
        self.fb = open(FBDEV, 'wb')
        self.out_buf = np.empty((H, W, 4), dtype=np.uint8)  # BGRA format

        self.write_state()
        self.commands = CommandReceiver(MIXER_COMMAND_SOCKET, 'mixer')
        self.patch_inbox = FrameInbox('mixer',self._prepare_patch,self._apply_patch)

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
                    'luma_feather': self.luma_feather,
                    'luma_high': self.luma_high,
                    'luma_high_enabled': self.luma_high_enabled,
                    'ext_usb_available': resolve_ext_usb_device() is not None,
                }, f)
        except:
            pass

    def get_source_frame(self, src):
        if src == SRC_CLIPS and self._shader_active and self._clip_stutter_frame is not None:
            return self._clip_stutter_frame
        t = time.perf_counter()
        if self.trace_selection and src==SRC_CLIPS:
            frame,origin=self.clip_player.get_trace_frame()
            self.trace_selection.remember(src,origin)
        elif self.trace_selection and 1<=src<=4:
            frame,origin=self.captures[src-1].get_trace_frame()
            self.trace_selection.remember(src,origin)
        else:
            if src == SRC_CLIPS:
                frame, event = self.clip_player.get_frame_state()
                self._clip_event_for_mix = event
            else:
                frame = self._get_source_frame(src)
            if self.trace_selection:
                origin=self.trace_selection.source(src,frame)
                if (6 <= src <= 15 and getattr(self, ('moire','quasi','hypno','circuit','smpte','ancient','shati','alien','slices','mandelbrodt')[src-6], None) is None) or (src in self.extra_shaders and self.extra_shaders[src] is None):
                    origin.update(state='unavailable',sequence=0,published_ns=0)
                self.trace_selection.remember(src,origin)
        if src == SRC_CLIPS and self._shader_active:
            frame = self.clip_stutter.apply(frame, self.clip_player.stutter_lengths)
            self._clip_stutter_frame = frame
        self.stage_ms['source_' + str(src)] = self.stage_ms.get('source_' + str(src), 0) + (time.perf_counter()-t)*1000
        return frame

    def _get_source_frame(self, src):
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
        elif src == SRC_EXT_USB:
            return self.captures[3].get_frame()
        elif src == SRC_SQUIRCLE:
            return self.squircle.render()
        elif src == SRC_MOIRE and self.moire is not None:
            return self.moire.render()
        elif src == SRC_QUASI and self.quasi is not None:
            return self.quasi.render()
        elif src == SRC_HYPNO and self.hypno is not None:
            return self.hypno.render()
        elif src == SRC_CIRCUIT and self.circuit is not None:
            return self.circuit.render()
        elif src == SRC_SMPTE and self.smpte is not None:
            return self.smpte.render()
        elif src == SRC_ANCIENT and self.ancient is not None:
            return self.ancient.render()
        elif src == SRC_SHATI and self.shati is not None:
            return self.shati.render()
        elif src == SRC_ALIEN and self.alien is not None:
            return self.alien.render()
        elif src == SRC_SLICES and self.slices is not None:
            return self.slices.render()
        elif src == SRC_MANDELBRODT and self.mandelbrodt is not None:
            return self.mandelbrodt.render()
        elif src in self.extra_shaders:
            return self.extra_shaders[src].render()
        return self.dummy.copy()

    def _prepare_patch(self, payload):
        keys=('ch_a','ch_b','mix','luma','luma_high','luma_src','luma_high_enabled')
        if set(payload)!=set(keys):raise ValueError('incomplete mixer patch')
        from command_ipc import validate_command
        for key,value in payload.items():
            validate_command({'action':'luma_high_en' if key=='luma_high_enabled' else key,'value':value},'mixer')
        if payload['luma']>payload['luma_high']:raise ValueError('luma threshold pair')
        return dict(payload),{key:getattr(self,key) for key in keys}

    def _apply_patch(self, payload):
        candidate,_=self._prepare_patch(payload)
        for key,value in candidate.items():setattr(self,key,value)
        self.write_state()
        self._update_active_captures()

    def handle_cmd(self, cmd, defer=False):
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

        elif action == 'luma_feather':
            self.luma_feather = max(0,min(4,int(cmd.get('value',1))))

        elif action == 'luma_src':
            # Set luma key source
            self.luma_src = cmd.get('value', 0)

        elif action == 'luma_high':
            # Set luma key high threshold
            # Constrain to not go below low threshold
            val = cmd.get('value', 127)
            self.luma_high = max(val, self.luma)

        elif action == 'luma_high_en':
            # Enable/disable high threshold cut
            self.luma_high_enabled = cmd.get('value', False)

        elif action == 'mosh':
            # Set datamosh amount (0-127)
            # 0 = off, 127 = full mosh
            # Map 0-127 to k range: 127->1.0 (bypass), 0->0.02 (full mosh)
            val = cmd.get('value', 0)
            if val == 0:
                self.mosh_enabled = False
                self.mosh_k = 1.0
            else:
                self.mosh_enabled = True
                # Log mapping: k = exp(lerp(log(1.0), log(0.02), val/127))
                import math
                t = val / 127.0
                self.mosh_k = math.exp((1.0 - t) * math.log(1.0) + t * math.log(0.02))

        elif action == 'shader_patch':
            self.shader_patch = validate_patch(cmd['value'])

        elif action == 'shader_fft':
            self.shader_bands = tuple(cmd['value'])
            self.shader_fft_at = time.monotonic()

        elif action == 'force_offset':
            self.force_cc[cmd['cc']] = cmd['value']


        if not defer:
            self.write_state()
            self._update_active_captures()

    def _update_active_captures(self):
        """
        Enable only the capture channels that are currently in use.

        This saves CPU by not running ffmpeg for unused sources.
        """
        # Determine which capture indices are needed
        needed = set()
        mix, luma, _, _ = self.effective_mixer_values()

        # Map source constants to capture indices (0-3)
        src_to_capture = {
            SRC_COMPOSITE1: 0,
            SRC_COMPOSITE2: 1,
            SRC_COMPOSITE3: 2,
            SRC_EXT_USB: 3,
        }

        # Channel A always needed
        if self.ch_a in src_to_capture:
            needed.add(src_to_capture[self.ch_a])

        # Channel B needed if mix > 0 or luma keying active
        if mix > 0 or luma > 0:
            if self.ch_b in src_to_capture:
                needed.add(src_to_capture[self.ch_b])

        # Luma source needed if luma keying active
        if luma > 0 and self.luma_src > 0:
            # luma_src is 1-indexed (1=clips, 2=comp1, etc), adjust to capture index
            luma_capture = self.luma_src - 2  # -2 because src 2 = capture 0
            if 0 <= luma_capture < len(self.captures):
                needed.add(luma_capture)

        # Update active flags
        for i, cap in enumerate(self.captures):
            cap.active = (i in needed)

    def _composite_gpu(self):
        from gpu_compositor import GpuCompositor
        if not hasattr(self, 'gpu_compositor'):
            self.gpu_compositor = GpuCompositor(W,H)
        gpu_sources = {5:self.squircle, 6:self.moire, 7:self.quasi,
                       8:self.hypno,9:self.circuit,10:self.smpte,
                       11:self.ancient,12:self.shati,13:self.alien,
                       14:self.slices,15:self.mandelbrodt}
        gpu_sources.update(self.extra_shaders)
        controls = self.current_shader_controls
        frame_sources = {}
        used_gpu = set()
        rendered_gpu = []
        def source(index, role):
            gpu = gpu_sources.get(index)
            if gpu is None or not hasattr(gpu, 'render_texture'):
                if index not in frame_sources:
                    frame_sources[index] = self.get_source_frame(index)
                return frame_sources[index]
            key = (index, round(self.shader_phase[role], 6),
                   round(controls[role][1], 6), round(controls[role][2], 6))
            if key in frame_sources:
                return frame_sources[key]
            if index in used_gpu:
                clone_key = (index, role)
                if clone_key not in self.shader_role_clones:
                    if isinstance(gpu, EyeOfEzbhanGpuSource):
                        clone = EyeOfEzbhanGpuSource(
                            gpu.source_code, gpu.field_source_code, gpu.width,
                            gpu.height, gpu.output_width, gpu.output_height)
                    elif isinstance(gpu, VoxelGpuSource):
                        clone = VoxelGpuSource(
                            gpu.source_code, gpu.width, gpu.height,
                            gpu.output_width, gpu.output_height)
                    elif isinstance(gpu, PolyMangleGpuSource):
                        clone = PolyMangleGpuSource(
                            gpu.vertex_code, gpu.source_code, gpu.width,
                            gpu.height, gpu.output_width, gpu.output_height)
                    else:
                        clone = GpuShaderSource(
                            gpu.name, gpu.source_code, gpu.width, gpu.height,
                            gpu.loop_seconds, gpu.output_width, gpu.output_height)
                    self.shader_role_clones[clone_key] = clone
                gpu = self.shader_role_clones[clone_key]
            used_gpu.add(index)
            rendered_gpu.append((index, gpu))
            t = time.perf_counter()
            if index in (26, 27):
                gpu.audio_bands = self.shader_bands
            frame_sources[key] = gpu.render_texture(
                time_override=self.shader_phase[role], controls=controls[role])
            if self.trace_selection:
                self.trace_selection.remember(index, self.trace_selection.source(
                    index, frame_sources[key], 'gpu_submit', getattr(gpu, 'last_render', None)))
            self.stage_ms['source_{}_{}'.format(index, role)] = (
                time.perf_counter() - t) * 1000
            return frame_sources[key]
        mix, luma, luma_high, feather = self.effective_mixer_values()
        a=source(self.ch_a, 0)
        b=source(self.ch_b, 2) if mix>0 else a
        key=source(self.luma_src-1, 1) if luma>0 and self.luma_src>0 else a
        frame = self.gpu_compositor.compose(a,b,key,mix,
            luma if self.luma_src>0 else 0,luma_high,self.luma_high_enabled,feather)
        self.stage_ms.update(self.gpu_compositor.profiler.collect())
        for index, gpu in rendered_gpu:
            if hasattr(gpu, 'profiler'):
                for stage, value in gpu.profiler.collect().items():
                    self.stage_ms['source_'+str(index)+'_'+stage] = value
        return frame

    def composite(self):
        self._clip_event_for_mix = None
        self._advance_shader_state()
        mix, luma, _, _ = self.effective_mixer_values()
        sources=(self.ch_a,self.ch_b if mix>0 else -1,
                 self.luma_src-1 if luma>0 else -1)
        was_shader_active = self._shader_active
        self._shader_active = any(index>=5 for index in sources)
        if was_shader_active and not self._shader_active:
            self.clip_stutter = OutputStutter()
        self._clip_stutter_frame = None
        if self._shader_active:
            return self._composite_gpu()
        return self._composite_cpu()

    def _advance_shader_state(self):
        now = time.monotonic()
        if self.shader_fft_at and now - self.shader_fft_at > 0.25:
            self.shader_bands = tuple(value * 0.9 for value in self.shader_bands)
        self.current_shader_controls = [
            effective_values(self.shader_patch, role, self.shader_bands,
                             self.force_cc)
            for role in range(3)]

        base_time = self.shader_beat_repeat.shader_elapsed(self.shader_clock_origin,
                                                            now=now)
        if self.shader_clock_last is None:
            self.shader_clock_last = base_time
        delta = max(-2.0, min(2.0, base_time - self.shader_clock_last))
        self.shader_clock_last = base_time
        for role in range(3):
            speed = 2.0 ** ((self.current_shader_controls[role][0]
                             - 64.0 / 127.0) * 2.0)
            self.shader_phase[role] = max(0.0, self.shader_phase[role] + delta * speed)

    def effective_mixer_values(self):
        mix = round(bounded(self.mix, self.force_cc[42], 127, 0, 127))
        low = round(bounded(self.luma, self.force_cc[43], 127, 0, 127))
        high = round(bounded(self.luma_high, self.force_cc[44], 127, 0, 127))
        feather = round(bounded(self.luma_feather, self.force_cc[45], 4, 0, 4))
        return mix, min(low, high), max(low, high), feather

    def _composite_cpu(self):
        """
        Perform frame compositing.

        Combines channel A and B with crossfade, then optionally
        applies luma key overlay from a third source.

        Returns:
            numpy array of shape (H, W, 3) - composited RGB frame
        """
        # Optimization: only fetch frames we need
        mix, luma, luma_high, feather = self.effective_mixer_values()
        need_a = True
        need_b = mix > 0 or luma > 0

        if self.trace_selection: self.trace_selection.role='A'
        frame_a = self.get_source_frame(self.ch_a) if need_a else self.dummy
        if self.trace_selection: self.trace_selection.role='B'
        frame_b = self.get_source_frame(self.ch_b) if need_b else self.dummy

        # --- Fast paths for pure A or B ---
        if mix == 0 and luma == 0:
            return frame_a

        if mix == 127 and luma == 0:
            return frame_b

        # --- Crossfade blend ---
        if mix > 0:
            mix_f = mix / 127.0  # Normalize to 0-1
            # Equal-power crossfade: both sources retain about 70.7% gain at
            # center instead of the dull 50%/50% produced by a linear fade.
            angle = mix_f * math.pi * 0.5
            gain_a = math.cos(angle)
            gain_b = math.sin(angle)
            cv2.addWeighted(frame_a, gain_a, frame_b, gain_b, 0,
                            dst=self.blend_out)
            out = self.blend_out
        else:
            out = frame_a

        # --- Luma key overlay ---
        if luma > 0 and self.luma_src > 0:
            # Get key source frame (source indices are 1-based, adjust to 0-based)
            if self.trace_selection: self.trace_selection.role='luma'
            key_frame = self.get_source_frame(self.luma_src - 1)

            # Convert to grayscale for threshold
            gray = cv2.cvtColor(key_frame, cv2.COLOR_RGB2GRAY)

            # Scale thresholds from 0-127 to 0-254 range
            luma_low_u8  = int(luma * 2)
            luma_high_u8 = int(luma_high * 2)

            if self.luma_high_enabled:
                # Band key: show key_frame only where brightness is between thresholds
                _, mask_low  = cv2.threshold(gray, luma_low_u8,  255, cv2.THRESH_BINARY)
                _, mask_high = cv2.threshold(gray, luma_high_u8, 255, cv2.THRESH_BINARY)
                mask = cv2.bitwise_and(mask_low, cv2.bitwise_not(mask_high))
            else:
                # Low key only: show key_frame where brightness exceeds threshold
                _, mask = cv2.threshold(gray, luma_low_u8, 255, cv2.THRESH_BINARY)

            # Apply mask - where mask is white, use key_frame; else use blended output
            if feather>0:
                from gpu_compositor import feather_cpu
                out=feather_cpu(out,key_frame,gray,luma,luma_high,
                                self.luma_high_enabled,feather)
            else:
                out = out.copy()
                cv2.copyTo(key_frame, mask, out)

        return out

    def datamosh(self, frame):
        """
        Apply datamosh effect - recursive motion-vector advection.

        When enabled (k < 1.0), warps an accumulator buffer using estimated
        motion vectors, blending live video based on k value.

        Args:
            frame: Current composited frame (H, W, 3) RGB

        Returns:
            Processed frame with datamosh effect applied
        """
        # Bypass if disabled or k is 1.0 (full passthrough)
        if not self.mosh_enabled or self.mosh_k >= 0.99:
            self.mosh_prev = frame.copy()
            return frame

        # Lazy init accumulator on first use
        if self.mosh_accum is None:
            self.mosh_accum = frame.astype(np.float32)
            self.mosh_prev = frame.copy()
            return frame

        # Convert to grayscale for motion estimation
        gray_curr = cv2.cvtColor(frame, cv2.COLOR_RGB2GRAY)
        gray_prev = cv2.cvtColor(self.mosh_prev, cv2.COLOR_RGB2GRAY)

        # Block-based motion estimation (16x16 blocks, simplified)
        block_size = 16
        search_range = 16
        h, w = gray_curr.shape
        blocks_y = h // block_size
        blocks_x = w // block_size

        # Create motion vector field (downsampled)
        mv_x = np.zeros((blocks_y, blocks_x), dtype=np.float32)
        mv_y = np.zeros((blocks_y, blocks_x), dtype=np.float32)

        # Simple block matching (SAD)
        for by in range(blocks_y):
            for bx in range(blocks_x):
                y0 = by * block_size
                x0 = bx * block_size
                block = gray_curr[y0:y0+block_size, x0:x0+block_size]

                best_sad = float('inf')
                best_dx, best_dy = 0, 0

                # Search in previous frame
                for dy in range(-search_range, search_range + 1, 4):  # Step by 4 for speed
                    for dx in range(-search_range, search_range + 1, 4):
                        py = y0 + dy
                        px = x0 + dx
                        if py < 0 or px < 0 or py + block_size > h or px + block_size > w:
                            continue
                        ref = gray_prev[py:py+block_size, px:px+block_size]
                        sad = np.sum(np.abs(block.astype(np.int16) - ref.astype(np.int16)))
                        if sad < best_sad:
                            best_sad = sad
                            best_dx, best_dy = dx, dy

                mv_x[by, bx] = best_dx / w  # Normalize to UV space
                mv_y[by, bx] = best_dy / h

        # Upscale motion vectors to full resolution
        mv_x_full = cv2.resize(mv_x, (w, h), interpolation=cv2.INTER_LINEAR)
        mv_y_full = cv2.resize(mv_y, (w, h), interpolation=cv2.INTER_LINEAR)

        # Create sampling coordinates for warp
        y_coords, x_coords = np.mgrid[0:h, 0:w].astype(np.float32)
        map_x = (x_coords - mv_x_full * w).astype(np.float32)
        map_y = (y_coords - mv_y_full * h).astype(np.float32)

        # Clamp to valid range
        map_x = np.clip(map_x, 0, w - 1)
        map_y = np.clip(map_y, 0, h - 1)

        # Warp accumulator using motion vectors
        warped = cv2.remap(self.mosh_accum, map_x, map_y, cv2.INTER_LINEAR)

        # Blend: out = warped + k * (live - warped)
        k = self.mosh_k
        self.mosh_accum = warped + k * (frame.astype(np.float32) - warped)

        # Slight saturation/contrast boost to counter bilinear softening
        self.mosh_accum = self.mosh_accum * 1.02
        self.mosh_accum = np.clip(self.mosh_accum, 0, 255)

        # Update previous frame
        self.mosh_prev = frame.copy()

        return self.mosh_accum.astype(np.uint8)

    def write_frame(self, frame):
        """
        Output a frame to framebuffer and TCP stream.

        Converts RGB to BGRA for framebuffer, sends RGB to ffmpeg.

        Args:
            frame: numpy array of shape (H, W, 3) with RGB pixel data
        """
        # Convert RGB to BGRA for framebuffer
        cv2.cvtColor(frame, cv2.COLOR_RGB2BGRA, dst=self.out_buf)

        # Write to the optional local composite preview. KMS can briefly expose
        # a fallback-sized framebuffer during mode changes; never let that
        # interrupt the network/show stream.
        if self.fb is not None:
            try:
                self.fb.write(self.out_buf.tobytes())
                self.fb.seek(0)  # Seek back to start for next frame
            except OSError as error:
                print('Framebuffer preview disabled: {}'.format(error),
                      flush=True)
                try:
                    self.fb.close()
                except OSError:
                    pass
                self.fb = None

        # Copy once into an immutable, owned frame; the worker owns all I/O.
        if self.trace_selection:
            metadata=self.trace_selection.metadata(self)
            self.stream_writer.submit_tagged(frame.tobytes(),self.trace_selection.session,self.trace_selection.sequence,metadata)
        else:
            self.stream_writer.submit(frame.tobytes())


    def _start_stream(self):
        """Keep the existing format and ffmpeg command; isolate pipe stalls."""
        from stream_writer import StreamWriter
        if self.trace_selection:
            if STREAM_FORMAT!='yuv420p':raise ValueError('Trace candidate requires current raw I420 format')
            self.stream_writer=trace_runtime.TraceWriter(STREAM_HOST,int(os.environ.get('AUTOEYEZ_TRACE_PORT','1247')))
            return
        cmd = [
            'ffmpeg', '-loglevel', 'warning',
            '-f', 'rawvideo', '-pix_fmt', 'rgb24',
            '-video_size', '{}x{}'.format(W, H),
            '-framerate', '30',
            '-i', 'pipe:0',
            '-vf', 'scale=720:480',
        ]
        if STREAM_FORMAT == 'yuv420p':
            cmd += ['-pix_fmt', 'yuv420p', '-c:v', 'rawvideo',
                    '-f', 'rawvideo']
        else:
            cmd += ['-pix_fmt', 'yuv420p', '-c:v', 'libx264',
                    '-preset', 'ultrafast', '-tune', 'zerolatency',
                    '-x264-params', 'keyint=30:min-keyint=30:intra-refresh=1:scenecut=0:sliced-threads=1:rc-lookahead=0:sync-lookahead=0:aud=1',
                    '-crf', '23', '-f',
                    'h264' if STREAM_FORMAT == 'h264_annexb' else 'mpegts']
        cmd += ['tcp://{}:{}?tcp_nodelay=1'.format(STREAM_HOST, STREAM_PORT)]
        self.stream_writer = StreamWriter(cmd, W * H * 3)
        print('{} stream writer started on {}:{}'.format(
            STREAM_FORMAT, STREAM_HOST, STREAM_PORT), flush=True)

    def start(self):
        """
        Start all capture channels and background threads.

        Only captures that are currently in use will actually run ffmpeg.
        """
        time.sleep(5)  # Wait for system to settle after boot

        # Determine which captures are needed based on initial state
        self._update_active_captures()

        # Start capture channel threads (they'll only capture when active)
        for ch in self.captures:
            ch.start()

        # Wait briefly for active captures to initialize
        time.sleep(2)

        # Start background threads
        threading.Thread(target=self._watchdog, daemon=True).start()

    def _watchdog(self):
        """
        Background thread: monitor capture channels for stalls.

        Checks each channel's last_frame_time and restarts ffmpeg
        if frames stop arriving. Uses different timeouts for initial
        connection vs. established streams.
        """
        while True:
            time.sleep(2)  # Check more frequently for faster hot-swap

            for ch in self.captures:
                # Skip inactive captures
                if not ch.active:
                    continue

                age = time.time() - ch.last_frame_time

                # Use shorter timeout for faster hot-swap recovery
                threshold = 5 if not ch.first_frame else 8

                if age > threshold:
                    print("Watchdog restarting capture: {} (age={:.1f}s)".format(
                        ch.device, age), flush=True)

                    # Reset state and kill process
                    ch.first_frame = False
                    if ch.process:
                        ch.process.kill()  # SIGKILL for immediate stop
                        try:
                            ch.process.wait(timeout=1)
                        except:
                            pass
                        ch.process = None

                    # Push deadline forward to allow restart
                    ch.last_frame_time = time.time() + 10

    def run(self):
        """
        Main run loop - start captures and composite at 30fps.

        This method never returns - it's the main entry point.
        """
        self.start()
        gpu_profile.refresh()

        target_fps = 30
        frame_time = 1.0 / target_fps
        perf_start = time.perf_counter()
        samples = []
        stage_totals = {}
        deadline = time.perf_counter()
        while True:
            t0 = time.perf_counter()
            # This thread owns compositor state. Apply the pending batch before
            # rendering; publish state and update captures only once per batch.
            commands = self.commands.drain()
            for cmd in commands:
                self.handle_cmd(cmd, defer=True)
            if commands:
                self.write_state()
                self._update_active_captures()
            self.patch_inbox.poll()
            commands_done = time.perf_counter()
            self.stage_ms = {}
            self.stage_ms['commands'] = (commands_done-t0)*1000
            self.shader_beat_repeat.poll()
            if self.trace_selection: self.trace_selection.begin()
            frame = self.composite()
            if self.trace_selection: self.trace_selection.end()
            t1 = time.perf_counter()
            frame = self.datamosh(frame)
            t2 = time.perf_counter()
            frame = self.output_stutter.apply(
                frame, {} if self._shader_active else self.clip_player.stutter_lengths)
            t_stutter = time.perf_counter()
            self.write_frame(frame)
            event = self._clip_event_for_mix
            if (event and event.get('event_id') != self._last_clip_event_submitted
                    and not self.output_stutter.active and not self.clip_stutter.active):
                submitted_ns = time.monotonic_ns()
                sent_ns = event.get('sent_ns')
                if isinstance(sent_ns, int) and sent_ns <= submitted_ns:
                    print('CLIP_LATENCY stage=mixer_submit event={} elapsed_ms={:.1f} '
                          'publish_to_submit_ms={:.1f}'.format(
                              event['event_id'], (submitted_ns-sent_ns)/1e6,
                              (submitted_ns-event['published_ns'])/1e6), flush=True)
                self._last_clip_event_submitted = event.get('event_id')
            t3 = time.perf_counter()
            self.stage_ms['composite'] = (t1-commands_done)*1000
            self.stage_ms['mosh'] = (t2-t1)*1000
            self.stage_ms['stutter'] = (t_stutter-t2)*1000
            self.stage_ms['output'] = (t3-t_stutter)*1000
            elapsed = t3-t0
            samples.append(elapsed*1000)
            for key,value in self.stage_ms.items():
                stage_totals[key] = stage_totals.get(key,0) + value
            deadline += frame_time
            sleep_t = deadline-time.perf_counter()
            if sleep_t > 0:
                time.sleep(sleep_t)
            elif sleep_t < -frame_time:
                deadline = time.perf_counter()
            now = time.perf_counter()
            if now-perf_start >= 5:
                ordered = sorted(samples)
                n = len(samples)
                report = {'fps':n/(now-perf_start), 'work_mean_ms':sum(samples)/n,
                          'work_p95_ms':ordered[min(n-1,int(n*.95))],
                          'work_max_ms':ordered[-1],
                          'over_budget':sum(x>frame_time*1000 for x in samples),
                          'frames':n, 'stages_ms':{k:v/n for k,v in stage_totals.items()}}
                report['stream'] = self.stream_writer.stats()
                print('MIXER_PERF '+json.dumps(report), flush=True)
                samples=[]; stage_totals={}; perf_start=now
                gpu_profile.refresh()

# =============================================================================
# ENTRY POINT
# =============================================================================

if __name__ == '__main__':
    # Add home directory to path for local imports
    sys.path.insert(0, '/home/admin')
    from video_player import VideoPlayer

    # Start clip player in background thread
    player = VideoPlayer()
    restored = None
    restore_path = '/home/admin/autoeyez_perf_restore.json'
    if os.path.exists(restore_path):
        with open(restore_path) as f:
            restored = json.load(f)
        os.remove(restore_path)
        # Apply test-session restoration before the decoder thread starts.
        player.handle_cmd({'action':'play_loop','index':restored['player']['index'],
                           'loop':restored['player']['loop']})
    threading.Thread(target=player.run, daemon=True).start()

    # Wait for player to initialize
    time.sleep(2)
    if (restored is not None and restored['player'].get('duration', 0) > 0
            and 'time_pos' in restored['player']):
        player.seek(restored['player']['time_pos'] /
                    restored['player']['duration'])

    # Start mixer (blocks forever)
    mixer = Mixer(player)
    if restored is not None:
        for key in ('ch_a','ch_b','mix','luma','luma_src','luma_high','luma_high_enabled','luma_feather'):
            if key in restored['mixer']:
                setattr(mixer,key,restored['mixer'][key])
        mixer._update_active_captures()
        mixer.write_state()
    mixer.run()
