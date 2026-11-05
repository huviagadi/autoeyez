# AutoEyez architecture

Source review: 2026-11-05.

## Evidence and status

This map comes from firmware, application source, shaders, and service files in
this repository. Older notes, plans, and READMEs were not used to establish
the architecture. The tree holds multiple generations and staged copies.
The code shows what each variant does; it does not prove what is installed or
running on either Pi. Live device state was not checked for this revision.

## Signal flow

```text
Composite capture ─┐
MP4 clips ──────────┼─> autoclip mixer ── TCP video ──> autowaaave receiver
Generated sources ──┘      │                             │
                           │                       /tmp/video_shm
                           │                             │
Akai Force ── USB MIDI ─────┤                     openFrameworks GPU
                           │                      feedback and effects
                           │                             │
                           └─ UDP CC offsets ────────────┤──> HDMI

Teensy ── USB MIDI: parameters and FFT ───────────────────┘
Teensy ⇄ USB serial ⇄ video_bridge.py ⇄ UDP ⇄ autoclip
Audio line in ──> Teensy Audio Shield; HiFiBerry supports Pi FFT/HDMI loop
```

Software targets autoclip at `10.0.0.2` and autowaaave at `10.0.0.1`.
The Teensy connects to autowaaave, not directly to autoclip.

## Hardware and controller firmware

The `automidi/auto_midi_v0911/` firmware targets a Teensy 4.1. It declares
nine quadrature encoders on pins 30–47, nine 128×64 SPI OLED objects with
chip selects on pins 0–6, 9, and 14, twelve button bits read through 74HC165
registers, and eight LED outputs through a 74HC595. It initializes the
Teensy's SD card at CS 10. The source identifies SH1106 display hardware even
though it uses `Adafruit_SSD1306` objects.

The PJRC Audio Shield's SGTL5000 line input feeds `AudioAnalyzeFFT256`.
The firmware also has left/right I2S monitor paths. It derives low, mid,
and high bands and sends them as USB MIDI CC 110–112 plus a completed-frame
marker on CC 113; CC 114–119 carry extended band values. In this source,
`FFT_SERIAL_COMPAT` is zero. USB serial carries commands and returned state.
The controller has three pages for renderer, playback, mixer, patch, and
processing controls. The firmware source names 26 selectable video sources.

Video and audio patches are distinct. The current firmware code bounds each
patch cursor to 32 slots and uses `PATCHnn.TXT` and `AUDIOnn.TXT` on SD.
It implements a serial `PTX` exchange, patch validation, and recovery of
interrupted saves. Its clip-list buffer holds up to 64 names; this is separate
from the patch-slot count. Sources: `auto_midi_v0911.ino`,
`video_patch_schema.h`, `safe_patch_store.h`, and `patch_journal.h`.

## Source and mixer Pi: autoclip

`autoclip/mixer.py` composes 720×576 RGB frames. Its inputs are the
`video_player.py` clip player, three composite USB capture devices, an
optional external USB capture device, and generated sources. Capture is
decoded by FFmpeg workers addressed through stable `/dev/v4l/by-path/`
names. Workers run when their source is selected and retry after failure.
`math_sources.py`, `gpu_shader_sources.py`, and shader files implement
generated inputs.
The controller menu has 26 source entries, while this root `mixer.py` defines
and dispatches only indices 0–15. The extra entries are not supported by this
root mixer file. A different deployed mixer may supply them; that has not
been verified here.

`video_player.py` uses PyAV for MP4 decode. It reads `/home/admin/clips/`
and prefers a matching `/home/admin/clips_seek/` copy when present. It
handles clip selection, play/pause, position seek, and two stutter lanes.
The mixer applies an equal-power A/B crossfade and optional third-source
luma key with low and high thresholds. It also writes a local preview to
`/dev/fb0`.

`autoclip/video_control.py` receives text commands on UDP 5006, passes
playback and mixer settings through local IPC, and returns clip/mixer state
on UDP 5005. The mixer reads `/tmp/mixer_cmd.json` and publishes
`/tmp/mixer_state.json`; the player also has a Unix datagram socket for
direct cue, seek, and stutter control.

`setup_midi_gadget.sh` configures the Pi USB MIDI gadget.
`force_cc_forward.py` reads the Akai Force MIDI input, sends selected CCs
to autowaaave on UDP 5010, and sends clip cue/seek/stutter commands to the
local player. `side_keypad.py` handles cue and shader-source control, but
its `patch_transport` and `shader_controls` imports are absent from the
`autoclip/` root in this checkout; staged copies exist elsewhere. Its
deployed module set must be inspected before describing that runtime exactly.

## Video transport

Both implemented routes produce RGB 720×480 frames for the renderer through
the GStreamer shared-memory socket `/tmp/video_shm`.

| Route | Sender | Receiver | Evidence |
| --- | --- | --- | --- |
| Raw | FFmpeg scales the mix to 720×480 I420/YUV420p and sends TCP 1237 | `rawvideoparse` → RGB → `shmsink` | `autoclip/mixer.py`, `autoclip/video.service`, `Codex Port/v04/stream_receive_shm.service` |
| Codec damage | `mixer_codec_intra_refresh.py` encodes Annex B H.264 with AUD and intra refresh to TCP 1238 | `realtime_codec_damage.py` forwards to local TCP 1239; H.264 decoder → RGB → `shmsink` | `codec_mosh_trial_20261104/` scripts and service overrides |

The codec proxy can remove H.264 slices before decode. It reads Teensy FFT
MIDI and controls in `/dev/shm/codec_damage`. This is separate from the
renderer GPU motion-smear effect. The root `video.service` selects raw
TCP 1237; the codec route needs different scripts and service overrides.
The repository alone cannot establish which route is currently active.

## Renderer Pi: autowaaave

The Pi 5 renderer source in `Codex Port/v04/ofApp.cpp` reads the incoming
frame with `ofVideoGrabber`. A newer staged renderer at
`autowaaave/peek_menu_fix_20261026/ofApp.cpp` retains a 720×480 input,
1920×1080 canvas, and 30-frame `pastFrames` FBO ring. The main mixer
shader combines the incoming image and selected history frames for feedback,
luma key, geometric displacement, color changes, mirror/wrap, and temporal
filtering. Later FBO passes apply sharpening, pixelation, chromatic
aberration, color crush, and motion smear. Staged renderer code adds further
input, hold, and Peek processing. The resulting image goes to HDMI and back
into the history ring.

The renderer takes Teensy MIDI parameters directly and listens for the Force
offset layer on UDP 5010. Audio-band assignments modulate parameters. Pi 5
renderer variants consume complete Teensy FFT MIDI frames; older serial
FFT-file support remains in the source tree.

`autowaaave/video_bridge.py` opens Teensy USB serial at
`/dev/ttyACM0`, 115200 baud. It forwards ordinary lines to autoclip UDP
5006 and returns autoclip UDP 5005 state to the controller. It handles local
FFT/envelope and audio-pass commands, sends `START` after detecting the
renderer process, and listens on UDP 5007 for a Teensy reboot request.
`alsaloop.service` can route shared HiFiBerry capture to HDMI audio;
`hdmi_audio.py` toggles it from controller commands.

## Processes and deployment boundary

| Host | Repository unit or script | Responsibility |
| --- | --- | --- |
| autoclip | `video.service` → `mixer.py` | Player, capture, mix, preview, stream |
| autoclip | `video_control.py` | UDP control/state and local IPC |
| autoclip | `midi-gadget.service`, `force_cc_forward.service` | Force USB MIDI |
| autoclip | `side-keypad.service` | Cue and shader-source controls; root imports incomplete |
| autowaaave | `stream_receive_shm.service` variant | TCP receiver and shared memory |
| autowaaave | Pi 5 `autowaaave.service` variant | openFrameworks renderer |
| autowaaave | `video_bridge.service` | Teensy serial ↔ autoclip UDP |
| autowaaave | optional `codec_damage.service` | Compressed-stream damage proxy |
| autowaaave | `alsaloop.service`, `hdmi_volume_cc.service` | Optional HDMI audio copy and Force volume |

Root renderer files and some root units describe earlier arrangements;
Pi 5 units and newer source are also in `Codex Port/`, `pi5_port/`,
and dated staging directories. A file's presence here does not mean that
unit is enabled or that the same source is installed on a Pi.

## Update procedure

For future revisions, compare this map with the deployed scripts and binary,
firmware hash, `systemctl cat` output, active services, and selected
stream port. Record observed device state and date separately from source
variants. Update claims only from inspected code or device observations.
