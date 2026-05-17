# autowaaave — Notes

## Hardware
- Raspberry Pi 3b+
- HiFiBerry audio card (hw:0,0) — line in for FFT audio analysis
- HDMI output — displays processed video
- Teensy connected via USB serial: /dev/ttyACM0

## Network
- autowaaave IP: 10.0.0.1 (PI1)
- autoclip IP: 10.0.0.2 (PI2)

## Services (systemd)
| Service              | Role                                                         |
|----------------------|--------------------------------------------------------------|
| stream_receive       | ffmpeg TCP listener on :1236 → writes to /dev/video10 (v4l2 loopback) |
| autowaaave           | Runs AUTO_WAAAVE_4_5 OF app (depends on stream_receive)      |
| video_bridge         | Python bridge: serial ↔ UDP between Teensy and autoclip      |

## Boot / Startup Order
1. stream_receive starts (ffmpeg listens for video from autoclip on TCP:1236, pushes to /dev/video10)
2. autowaaave starts (OF app, depends on stream_receive)
3. video_bridge starts: connects serial, waits for AUTO_WAAAVE_4_5 process, signals Teensy "START", bridges bidirectional serial↔UDP

---

## video_bridge.py — Serial/UDP Bridge
- Serial: /dev/ttyACM0 @ 115200 baud (Teensy USB)
- Receives state updates from autoclip UDP:5005 → writes to Teensy serial
- Receives serial from Teensy → forwards to autoclip UDP:5006
- Listens on UDP:5007 for REBOOT command (from TeensyFlaSSH flashing workflow)
- Handles MIDI CC7 volume via `mido` → `amixer set Master` (maps 0–100 linear, 100–127 boosted to 150%)
- Detects AUTO_WAAAVE_4_5 process running, waits 5s, then sends "START\n" to Teensy

---

## AUTO_WAAAVE_4_5 — openFrameworks App

### What It Does
Receives the video stream from autoclip, processes it through a GPU shader feedback pipeline, and outputs to HDMI. The core is a self-modulating video feedback loop with real-time control over spatial transforms, color manipulation, temporal mixing, and audio reactivity.

### Resolution / Framerate
- 640×480 @ 30fps (scaleswitch=1 mode)
- framebufferLength = 60 frames = 2 seconds of delay buffer

### Pipeline Per Frame (draw())
```
cam1 (video from /dev/video10)
    ↓
framebuffer0 ← shader_mixer (see uniforms below)
    ↓
sharpenFramebuffer ← shaderSharpen
    ↓
HDMI output (sharpenFramebuffer.draw to screen)
    ↓
pastFrames ring buffer ← write current frame (wet) or raw cam (dry)
```

### FBOs
| FBO                | Purpose                                                      |
|--------------------|--------------------------------------------------------------|
| framebuffer0       | Primary processing buffer — shader_mixer output              |
| pastFrames[60]     | Circular ring buffer — stores past frames for delay/feedback |
| dry_framebuffer    | Raw camera copy — used as temporal filter source in dry mode |
| sharpenFramebuffer | Post-sharpening stage before HDMI output                     |
| aspect_fix_fbo     | Optional 853×480 crop to fill 640×480 for HD aspect ratio    |

### Wet/Dry Mode
- **Wet (1):** pastFrames is written with the processed output → true feedback loop, self-modulates
- **Dry (0):** pastFrames is written with raw camera input → traditional video delay line

---

## Shader Parameters (AUTO_WAAAVE shader_mixer)

All parameters are the sum of three layers:
`d_param = base_value + (coefficient × p_lock_smoothed[n]) + (4.0 × audio_coefficient × fft_band_smoothed)`

| Parameter             | Shader Uniform         | MIDI CC | Type     | Description                              |
|-----------------------|------------------------|---------|----------|------------------------------------------|
| Luma Key              | lumakey                | CC16    | unipolar | Threshold for luma keying fb over camera |
| FB Mix                | fbMix                  | CC17    | bipolar  | Mix between camera and feedback frame    |
| Hue                   | fbHue                  | CC18    | bipolar  | Hue rotation of feedback frame           |
| Saturation            | fbSaturation           | CC19    | bipolar  | Saturation of feedback frame             |
| Brightness            | fbBright               | CC20    | bipolar  | Brightness of feedback frame             |
| Temporal Filter Mix   | temporalFilterMix      | CC21    | bipolar  | Blend between current and temporal frame |
| Temporal Resonance    | temporalFilterResonance| CC22    | unipolar | Brightness/intensity of temporal filter  |
| Sharpen               | sharpenAmount          | CC23    | unipolar | Sharpening kernel strength               |
| X Displace            | fbXDisplace            | CC120   | bipolar  | Horizontal pixel displacement of fb      |
| Y Displace            | fbYDisplace            | CC121   | bipolar  | Vertical pixel displacement of fb        |
| Z Displace (zoom)     | fbZDisplace            | CC122   | bipolar  | Zoom/scale of feedback frame             |
| Rotate                | fbRotate               | CC123   | bipolar  | Rotation of feedback frame               |
| Hue Modulo            | fbHuexMod / vHuexMod   | CC124   | unipolar | Hue chaos — modulo divisor               |
| Hue Offset            | fbHuexOff / vHuexOff   | CC125   | bipolar  | Hue chaos — static offset                |
| Hue LFO               | fbHuexLfo / vHuexLfo   | CC126   | bipolar  | Hue chaos — oscillation rate             |
| Delay Time            | (index into pastFrames)| CC127   | unipolar | How far back in ring buffer to read      |
| Audio Input Gain      | (inputGain)            | CC7     | unipolar | FFT sensitivity (maps to 0.1–4.0)        |

### Boolean Switches (MIDI CC)
| CC  | Function             | Notes                                  |
|-----|----------------------|----------------------------------------|
| CC41 | Horizontal Mirror   |                                        |
| CC45 | Vertical Mirror     |                                        |
| CC46 | Toroid Switch       | Wraps displaced pixels at edges        |
| CC59 | Full Reset          | Zeros all v*, audio attenuators, p_lock|
| CC60 | Luma Key Invert     | **Bug: also sets brightInvert (CC60 duplicate in boolean section)** |
| CC61 | Mirror Switch       | **Bug: also sets saturationInvert (CC61 duplicate in boolean section)** |
| CC62 | Hue Invert          |                                        |
| CC71 | Wet/Dry Switch      | 127=dry, 0=wet                         |
| CC90 | HDMI Aspect Ratio   | 0=SD pillarbox, 1=crop to fill         |

### Encoder Multipliers
Applied to x/y/z displace and rotation p_lock values:
| CC group        | Parameters | Multipliers          |
|-----------------|------------|----------------------|
| CC32/48/64      | X displace | 2×, 5×, 10×          |
| CC33/49/65      | Y displace | 2×, 5×, 10×          |
| CC34/50/66      | Z displace | 2×, 5×, 10×          |
| CC35/51/67      | Rotate     | 2×, 5×, 10×          |
| CC36/52/68      | Hue modulo | /64, /96, /127 (finer control vs default /32) |
| CC37/53/69      | Hue offset | 2×, 4×, 8×           |
| CC38/54/70      | Hue LFO    | 2×, 4×, 8×           |

### Audio Reactive System
- FFT via ofxProcessFFT on HiFiBerry line in
- Three bands: low, mid, high (smoothed at rate 0.25)
- `audioReactiveControlSwitch`: 0=direct MIDI, 1=low, 2=mid, 3=high
  - CC43=127 → low band active
  - CC44=127 → mid band active
  - CC42=127 → high band active
  - Any band=0 → switch back to 0 (direct)
- When active, each MIDI CC writes an **attenuator value** (lowCn/midCn/highCn) for that parameter
- Attenuator is multiplied by the smoothed FFT band value and added to the parameter

### P-Lock (Parameter Lock / Sequencer)
- 17 parameters × 240 steps circular buffer (float p_lock[17][240])
- Steps advance one per frame (at 30fps = 8 seconds per cycle)
- Smoothed with factor 0.5 before applying
- When p_lock_switch=1 and a CC comes in within CONTROL_THRESHOLD of stored value, it writes to current step → records knob movements as a looping sequence
- **CC91=127 (forceMidiWrite):** bypass threshold, write all incoming CCs immediately — used during patch load sweep
- **CC91=0:** end force write, call fillPLockFromCurrent() which floods all steps with the last written value → "locks" current parameter state into loop
- **CC92=n:** save audio patch slot n to /home/pi/audiopatches/AUDIOn.TXT (48 values: lowC1-16, midC1-16, highC1-16 + react switch)
- **CC93=n:** load audio patch slot n

### Video Reactive Mode
- `videoReactiveSwitch` (CC39) — when on, knobs write to v* variables instead of p_lock
- v* variables are additional offsets applied alongside p_lock with their own scaling coefficients
- Allows a separate "video reactive" layer of parameter modulation

---

## FPGA / Nepenthes Implementation Notes

This section documents the shader operations for future HDL implementation.

The entire effect is a **per-pixel feedback loop**. Each output pixel is computed from:
- The current camera input pixel (at that UV coordinate)
- A pixel read from the delayed/feedback framebuffer (at a transformed UV coordinate)
- A pixel read from the temporal filter framebuffer

### Operations to implement in hardware:

**1. Luma Key**
- Convert feedback pixel to luminance (Y = 0.299R + 0.587G + 0.114B)
- Threshold compare → binary mask
- Select: mask=1 → camera pixel, mask=0 → feedback pixel
- Optional invert of mask (lumakeyInvertSwitch)

**2. HSB Color Transform (on feedback pixel)**
- RGB → HSV conversion
- Add hue offset (rotation of hue angle, modulo 1.0)
- Multiply saturation and value by scalars
- Optional per-channel invert (hueInvert, saturationInvert, brightInvert)
- HSV → RGB conversion

**3. Spatial Transform (UV coordinate warping for feedback read)**
- Translation: UV += (fbXDisplace, fbYDisplace)
- Zoom: UV = (UV - 0.5) / fbZDisplace + 0.5
- Rotation: apply 2D rotation matrix around center
- Toroidal wrap: UV = fract(UV) — wrap edges instead of clamp
- Mirror: UV.x = 1.0 - UV.x and/or UV.y = 1.0 - UV.y
- Bilinear interpolation for non-integer UV addresses

**4. Temporal Mix**
- Blend: output = mix(current_processed, temporal_frame_pixel, temporalFilterMix)
- temporal_frame_pixel fetched from ring buffer at offset = max delay index
- Resonance controls brightness of temporal frame

**5. Sharpen (post-process)**
- 3×3 convolution kernel (unsharp mask or Laplacian sharpening)
- sharpenAmount controls kernel strength

**6. Hue Chaos (huex system)**
- Hue modulo: hue = fract(hue × fbHuexMod)
- Hue offset: hue += fbHuexOff (with wrapping)
- Hue LFO: hue += sin(time × fbHuexLfo) (oscillating hue shift)
- All three applied additively to hue channel

**7. Circular Ring Buffer**
- 60 frames × 640×480 × 3 bytes = ~55MB in RAM
- Write pointer advances each frame
- Read pointer = write pointer - delay_amount (modulo 60)
- FPGA: implement as SDRAM with dual-port access (write current, read delayed)

**8. Wet/Dry Feedback Path**
- Wet: write processed (sharpened) frame into ring buffer → true feedback
- Dry: write raw camera frame into ring buffer → delay only

### Key insight for HDL design:
The shader is fundamentally a **UV-space feedback sampler with color transforms**. All operations happen per-pixel in the fragment shader. In FPGA, this maps naturally to a pipeline: read camera pixel → read delayed pixel → transform UV → bilinear fetch → color transform → luma key → temporal blend → sharpen → write output + write to ring buffer.

---

## Known Issues / Notes
- `midibizOld()` function exists but is never called — legacy code, safe to remove
- CC60 in midibiz() maps to both `lumakeyInvertSwitch` (toggle section ~L1210) and `brightInvert` (boolean section ~L2239) — both fire on the same CC, likely a copy-paste bug
- CC61 maps to both `mirrorSwitch` (~L1177) and `saturationInvert` (~L2243) — same issue
- Audio patches stored at /home/pi/audiopatches/ (7 patches currently: 01–05, 15, 63)
- OF app opens MIDI port 1 (hardcoded) — port 0 is typically the system default, port 1 is the Teensy
- `midibizOld()` still references old CC mapping conventions — do not use as reference

## Current Files
- ofApp.cpp / ofApp.h — openFrameworks application
- video_bridge.py — serial/UDP bridge
- autowaaave.service / stream_receive.service / video_bridge.service — systemd units
- asound.conf — ALSA config (HiFiBerry card)
- audiopatches_list.txt — list of saved audio patch files on Pi
