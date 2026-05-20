# autowaaave — GPU Shader Video Processor

Raspberry Pi 3B+ running openFrameworks for real-time GPU video feedback processing. The visual heart of the autoeyez system — receives video from autoclip, applies shader effects, outputs to HDMI.

**Based on [auto_waaave](https://github.com/ex-zee-ex/auto_waaave) by Andrei Jay (ex-zee-ex)** — forked from version 1.5 and modified for integration with the autoeyez control system.

## Features

- **GPU fragment shader** video feedback pipeline
- **Spatial transforms** — zoom, rotate, translate, mirror, toroidal wrap
- **Color manipulation** — hue shift, saturation, brightness, invert
- **Luma keying** — composite camera over feedback based on brightness
- **Temporal filtering** — blend with delayed frames
- **60-frame delay buffer** (2 seconds at 30fps)
- **Wet/dry feedback** — true feedback loop or simple delay
- **Audio-reactive modulation** via FFT (low/mid/high bands)
- **Sharpening** post-process
- **MIDI control** of all parameters

## Pipeline

```
Video from autoclip (TCP:1236)
        ↓
    /dev/video10 (v4l2loopback)
        ↓
    cam1 (OF VideoGrabber)
        ↓
    shader_mixer.frag
    - Read feedback frame from pastFrames[delay]
    - Apply spatial transform (zoom, rotate, translate)
    - Apply color transform (hue, sat, bright)
    - Luma key with camera
    - Temporal filter blend
        ↓
    shaderSharpen
        ↓
    HDMI Output
        ↓
    Write to pastFrames ring buffer
```

## Resolution

- Input: 720×480 from autoclip
- Processing: 640×480 @ 30fps (internal FBOs)
- Output: 1280×720 HDMI (upscaled)

## Shader Parameters

| Parameter | MIDI CC | Type | Description |
|-----------|---------|------|-------------|
| Luma Key | CC16 | unipolar | Threshold for keying |
| FB Mix | CC17 | bipolar | Camera ↔ feedback blend |
| Hue | CC18 | bipolar | Hue rotation |
| Saturation | CC19 | bipolar | Saturation adjust |
| Brightness | CC20 | bipolar | Brightness adjust |
| Temporal Mix | CC21 | bipolar | Current ↔ delayed blend |
| Temporal Res | CC22 | unipolar | Temporal intensity |
| Sharpen | CC23 | unipolar | Sharpening amount |
| X Displace | CC120 | bipolar | Horizontal offset |
| Y Displace | CC121 | bipolar | Vertical offset |
| Z Displace | CC122 | bipolar | Zoom/scale |
| Rotate | CC123 | bipolar | Rotation angle |
| Hue Modulo | CC124 | unipolar | Hue chaos divisor |
| Hue Offset | CC125 | bipolar | Hue chaos offset |
| Hue LFO | CC126 | bipolar | Hue oscillation |
| Delay Time | CC127 | unipolar | Frames back in buffer |

## Services

| Service | Purpose |
|---------|---------|
| stream_receive | Receives TCP stream, writes to v4l2loopback |
| autowaaave | Main openFrameworks application |
| video_bridge | Serial↔UDP bridge for automidi communication |

## Files

- `ofApp.cpp` / `ofApp.h` — openFrameworks application
- `video_bridge.py` — Serial/UDP bridge
- `autowaaave.service` — Main app service
- `stream_receive.service` — Video receiver service
- `video_bridge.service` — Bridge service
- `asound.conf` — ALSA config for HiFiBerry
- `config.txt` — Boot configuration
- `BUILDME.md` — Pi setup, HiFiBerry installation

## See Also

- `BUILDME.md` for Raspberry Pi and HiFiBerry setup
- `NOTES.md` for detailed shader parameter reference
- Parent `README.md` for system overview
