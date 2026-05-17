# autoclip — Notes

## Hardware
- Raspberry Pi 5
- 3 × composite capture cards: /dev/video0, /dev/video2, /dev/video4 (MJPEG, 640×480, 30fps)
- Framebuffer output: /dev/fb0
- Output resolution: 720×576

## Software Architecture — Three Python scripts

### mixer.py (main process — launched by video.service as root)
- Owns the compositing pipeline and framebuffer
- Spawns VideoPlayer and runs Mixer.run() in the main thread
- IPC: polls /tmp/mixer_cmd.json for commands from video_control.py

### video_player.py
- Manages clip playback via ffmpeg subprocess (rawvideo pipe)
- Clips stored at /boot/firmware/clips/ (*.mp4)
- IPC: polls /tmp/player_cmd.json, writes /tmp/player_state.json
- Tracks index, paused, loop, time_pos, duration

### video_control.py
- Bridges network commands → IPC cmd files
- Receives commands via UDP port 5006
- Sends state updates via UDP to 10.0.0.1 (autowaaave) port 5005
- Polls /tmp/player_state.json and sends FILE:, POS:, PAUSE:, LOOP:, PROGRESS: updates
- Resends full state (clip list + current state) every 10 seconds

## IPC Flow
```
automidi (serial) → [bridge on autowaaave] → UDP:5006 → video_control.py
                                                              ↓
                                          /tmp/player_cmd.json  or  /tmp/mixer_cmd.json
                                                              ↓
                                         video_player.py        mixer.py
                                                              ↓
                                          /tmp/player_state.json
                                                              ↓
                                         video_control.py → UDP:5005 → autowaaave → serial → automidi
```
Note: serial bridge (serial → UDP) likely lives in video_bridge.py on autowaaave — to be confirmed.

## Network
- autowaaave IP: 10.0.0.1
- autoclip streams video to autowaaave: tcp://10.0.0.1:1236 (H.264, mpegts, ~8Mbps)
- Stream: 720×576 captured → scaled 720×480, yuv420p, libx264, preset faster, crf 18
- Stream auto-reconnects on disconnect

## Sources (5 total)
| Index | Name         | Backend                        |
|-------|--------------|--------------------------------|
| 0     | CLIPS        | VideoPlayer frames             |
| 1     | COMPOSITE 1  | /dev/video0 (CaptureChannel)   |
| 2     | COMPOSITE 2  | /dev/video2 (CaptureChannel)   |
| 3     | COMPOSITE 3  | /dev/video4 (CaptureChannel)   |
| 4     | OSCILLOSCOPE | blank frame (disabled)         |

## Compositing Pipeline
1. Get frame_a (ch_a source) — always
2. Get frame_b (ch_b source) — only if mix > 0 or luma > 0
3. If mix > 0: cv2.addWeighted blend (mix/127.0)
4. If luma > 0 and luma_src > 0:
   - Get luma key source frame → convert to grayscale
   - Low threshold: luma * 2 (0–254)
   - High threshold (if luma_high_enabled): luma_high * 2
   - Band key (high enabled): show key_frame only between low and high thresholds
   - Low key only: show key_frame above low threshold
   - np.where mask applies key_frame over blended output
5. Write to /dev/fb0 (BGRA) and stream process stdin (RGB)

## Commands Handled (video_control.py → cmd files)
| Serial Command   | Action                          |
|------------------|---------------------------------|
| NEXT             | next clip                       |
| PREV             | prev clip                       |
| PLAY             | resume playback                 |
| PAUSE            | pause, save position            |
| LOOP_ON/OFF      | toggle loop                     |
| PLAY:n           | jump to clip index n            |
| CH_A:n           | set mixer channel A source      |
| CH_B:n           | set mixer channel B source      |
| MIX:n            | set crossfade value (0–127)     |
| LUMA:n           | set luma key low threshold      |
| LUMA_SRC:n       | set luma key source index       |
| LUMA_HIGH:n      | set luma key high threshold     |
| LUMA_HIGH_EN:n   | enable/disable high cut         |

## State Updates Sent → automidi
| Message              | Trigger                        |
|----------------------|--------------------------------|
| LIST:name1,name2,... | every 10s + on startup         |
| FILE:name            | on clip change                 |
| POS:index:count      | on clip change                 |
| PAUSE:0/1            | on pause state change          |
| LOOP:0/1             | on loop state change           |
| PROGRESS:pos:dur     | every ~1s during playback      |

## Watchdog
- Checks each CaptureChannel every 5 seconds
- Restarts ffmpeg capture if no frame received within 8s (pre-first-frame) or 15s (after)

## Clip Naming
- Files: sorted *.mp4 in /boot/firmware/clips/
- Display name: strip extension, strip prefix up to first underscore, uppercase
  - e.g. `01_coolclip.mp4` → `COOLCLIP`

## Systemd Service
- File: /etc/systemd/system/video.service
- Runs: `/usr/bin/python3 /home/admin/mixer.py`
- User: root (required for framebuffer access)
- Restart: always, 5s delay

## Capture Sources — Clarification
All 3 composite capture devices are real and working:
- COMPOSITE 1 → /dev/video0
- COMPOSITE 2 → /dev/video2
- COMPOSITE 3 → /dev/video4 (labeled "dummy" in code comment — incorrect, should be cleaned up)
Naming conventions to be cleaned up later.

## Known Issues / Bugs
- Duplicate `elif action == 'luma':` in mixer.py `handle_cmd()` — second block is dead code; first block wins but doesn't call `write_state()`
- OscilloscopeChannel instantiated but never started (`self.osc.start()` commented out) — SRC_OSCILLOSCOPE always returns blank frame
- `pad` flag in CaptureChannel is set True for all devices but vf string is identical regardless — no actual padding/letterbox logic implemented

## Potential Changes / TODOs
- Fix duplicate luma handler in mixer.py — consolidate into one block that calls write_state()
- Clean up SRC_COMPOSITE3 "dummy" label and comment — rename to reflect it's a real capture
- Decide on oscilloscope: either wire it up properly or remove the dead class
- Investigate whether pad=True should produce letterboxed 720×576 from 640×480 input (add padding bars vs. stretch)
- Consider replacing /tmp JSON file polling IPC with something lower-latency (e.g. Unix socket or named pipe) — current 10ms poll on cmd files adds latency
- Clip naming convention cleanup (prefix stripping logic is fragile)

## Current Files
- mixer.py — compositing, framebuffer write, stream to autowaaave
- video_player.py — clip playback engine
- video_control.py — command/state bridge (UDP ↔ IPC files)
- video.service — systemd unit
