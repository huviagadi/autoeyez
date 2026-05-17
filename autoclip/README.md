# autoclip — Video Clip Player & Mixer

Raspberry Pi 5-based video clip player and composite video mixer for the autoeyez system. Captures 3 composite video sources, plays MP4 clips, composites with crossfade and luma keying, and streams to autowaaave.

## Features

- **MP4 clip playback** from SD card with loop, pause, seek
- **3 composite capture inputs** via USB capture cards
- **Real-time compositing** — crossfade between any two sources
- **Luma keying** with adjustable low/high thresholds
- **TCP video streaming** to autowaaave (720×480 H.264)
- **UDP command interface** for remote control from automidi

## Video Sources

| Index | Source | Backend |
|-------|--------|---------|
| 0 | CLIPS | VideoPlayer (MP4 files) |
| 1 | COMPOSITE 1 | /dev/video0 capture |
| 2 | COMPOSITE 2 | /dev/video2 capture |
| 3 | COMPOSITE 3 | /dev/video4 capture |

## Architecture

Three Python scripts work together:

### mixer.py (main process)
- Owns the compositing pipeline
- Blends sources based on mix value
- Applies luma keying
- Writes to local framebuffer and TCP stream
- Polls command file for mixer settings

### video_player.py
- Manages MP4 playback via ffmpeg subprocess
- Handles play, pause, loop, seek, clip switching
- Writes state to JSON for status reporting

### video_control.py
- Bridges network commands to IPC files
- Receives UDP commands from automidi (via autowaaave bridge)
- Sends state updates back (clip list, position, status)

## Network

| Direction | Protocol | Port | Purpose |
|-----------|----------|------|---------|
| autoclip → autowaaave | TCP | 1236 | H.264 video stream |
| autowaaave → autoclip | UDP | 5006 | Commands |
| autoclip → autowaaave | UDP | 5005 | State updates |

**Static IP:** 10.0.0.2

## Commands

| Command | Action |
|---------|--------|
| NEXT / PREV | Navigate clips |
| PLAY / PAUSE | Playback control |
| LOOP_ON / LOOP_OFF | Toggle loop |
| PLAY:n | Jump to clip index n |
| CH_A:n / CH_B:n | Set mixer channel source |
| MIX:n | Crossfade value (0-127) |
| LUMA:n | Luma key low threshold |
| LUMA_HIGH:n | Luma key high threshold |
| LUMA_SRC:n | Luma key source index |
| LUMA_HIGH_EN:n | Enable/disable high cut |

## Clip Storage

Clips are stored at `/boot/firmware/clips/` as MP4 files.

Naming convention: `NN_CLIPNAME.mp4` where NN is sort order prefix.
Display name strips prefix and extension, converts to uppercase.

Example: `01_coolclip.mp4` → displays as "COOLCLIP"

## Files

- `mixer.py` — Main compositor, framebuffer output, stream
- `video_player.py` — Clip playback engine
- `video_control.py` — Command/state bridge
- `video.service` — systemd unit (runs mixer.py as root)
- `BUILDME.md` — Capture card details, Pi setup

## See Also

- `BUILDME.md` for capture card compatibility and setup
- Parent `README.md` for system overview
