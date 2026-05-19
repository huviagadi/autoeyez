# autoeyez — Modular Video Synthesis System

A three-component real-time video synthesis and feedback system built around Raspberry Pi and Teensy hardware. Designed for live visual performance with hands-on control.

## System Overview

autoeyez combines hardware video mixing, GPU shader processing, and tactile MIDI control into a unified instrument for creating psychedelic video feedback effects, clip playback, and live composite video manipulation.

```mermaid
flowchart TB
    subgraph automidi[automidi - Teensy 4.1]
        ENC[9 Encoders + OLEDs]
        BTN[12 Buttons + 8 LEDs]
        FFT[Audio FFT Display]
    end

    subgraph autowaaave[autowaaave - Pi 3B+]
        BRIDGE[video_bridge.py]
        SHADER[openFrameworks Shaders]
        AUDIO[HiFiBerry Line-In]
    end

    subgraph autoclip[autoclip - Pi 5]
        PLAYER[Clip Player]
        MIXER[Video Mixer]
        CAPTURE[3x Composite Capture]
    end

    subgraph external[External]
        SOURCES[Video Sources]
        HDMI[HDMI Output]
    end

    automidi -->|USB MIDI| SHADER
    automidi -->|USB Serial| BRIDGE
    BRIDGE -->|Serial to UDP| automidi

    BRIDGE -->|UDP Commands| autoclip
    autoclip -->|UDP State| BRIDGE
    autoclip -->|TCP Video Stream| SHADER

    SOURCES --> CAPTURE
    PLAYER --> MIXER
    CAPTURE --> MIXER
    MIXER -->|720x480 H.264| SHADER
    SHADER --> HDMI

    AUDIO --> SHADER
```

**Data Flow:**
- **MIDI CC** → Teensy sends shader parameters to autowaaave via USB MIDI
- **Commands** → Teensy sends clip/mixer commands through autowaaave's serial→UDP bridge to autoclip
- **State** → autoclip sends playback state through autowaaave's UDP→serial bridge back to Teensy OLEDs
- **Video** → autoclip streams composited video to autowaaave via TCP (port 1236)
- **Output** → autowaaave applies shader effects and outputs to HDMI

## Components

### automidi (Teensy 4.1)
Hardware MIDI controller with 9 rotary encoders, 9 OLED displays, 12 buttons, and 8 LEDs. Sends MIDI CC to autowaaave for shader control and serial commands to autoclip for clip/mixer control. Features audio-reactive FFT visualization, patch save/load, and multi-page parameter layouts.

### autowaaave (Raspberry Pi 3B+)
openFrameworks application running GPU fragment shaders for real-time video feedback processing. Receives video stream from autoclip, applies spatial transforms, color manipulation, luma keying, temporal filtering, and outputs to HDMI. Audio-reactive parameters modulated by FFT analysis of line input. **Based on [auto_waaave](https://github.com/ex-zee-ex/auto_waaave) by Andrei Jay.**

### autoclip (Raspberry Pi 5)
Video clip player and hardware mixer. Plays MP4 clips from SD card, captures 3 composite video sources via USB capture cards, composites them with crossfade and luma keying, and streams the result to autowaaave over TCP.

## Signal Flow

```mermaid
flowchart LR
    subgraph sources[Video Sources]
        COMP1[Composite 1]
        COMP2[Composite 2]
        COMP3[Composite 3]
        CLIPS[(MP4 Clips)]
    end

    subgraph autoclip[autoclip - Pi 5]
        CAP[USB Capture]
        PLAY[Clip Player]
        MIX[Video Mixer]
        ENC[H.264 Encode]
    end

    subgraph autowaaave[autowaaave - Pi 3B+]
        DEC[Decode Stream]
        FB[Feedback Buffer]
        SHADER[GPU Shaders]
        SHARP[Sharpen]
    end

    HDMI[HDMI Display]

    COMP1 --> CAP
    COMP2 --> CAP
    COMP3 --> CAP
    CLIPS --> PLAY

    CAP --> MIX
    PLAY --> MIX
    MIX --> ENC
    ENC -->|TCP 1236| DEC

    DEC --> FB
    FB --> SHADER
    SHADER --> SHARP
    SHARP --> HDMI
    SHADER -.-> FB
```

| Stage | Device | Process |
|-------|--------|---------|
| 1. Capture | autoclip | USB capture cards decode composite video (640×480 MJPEG) |
| 2. Playback | autoclip | MP4 clips decoded via ffmpeg (720×576) |
| 3. Mixing | autoclip | A/B crossfade + luma key compositing |
| 4. Streaming | autoclip | H.264 encode → TCP to autowaaave (720×480) |
| 5. Feedback | autowaaave | 60-frame circular buffer (2 sec delay) |
| 6. Shaders | autowaaave | UV displacement, HSB manipulation, temporal filter |
| 7. Sharpen | autowaaave | Optional sharpening post-process |
| 8. Output | autowaaave | Final frame to HDMI display |

## Features

- **GPU video feedback** with zoom, rotate, translate, hue shift
- **Luma keying** with adjustable threshold and source selection
- **Temporal filtering** with 60-frame delay buffer (2 seconds)
- **3 composite capture inputs** for external video sources
- **Clip playback** with loop, pause, and instant switching
- **9 parameter encoders** with visual feedback on OLEDs
- **32 video patches** + 32 audio-reactive patches (save/load)
- **Audio-reactive modulation** — FFT low/mid/high bands
- **Live performance ready** — boots to operational state

## Directory Structure

```
autoeyez/
├── README.md           # This file
├── BUILDME.md          # Hardware build overview
├── automidi/           # Teensy MIDI controller
│   ├── README.md
│   ├── BUILDME.md      # Detailed hardware build
│   └── auto_midi_v0911/
├── autoclip/           # Pi 5 clip player + mixer
│   ├── README.md
│   ├── BUILDME.md      # Capture cards, peripherals
│   └── *.py, *.service
├── autowaaave/         # Pi 3B+ shader processor
│   ├── README.md
│   ├── BUILDME.md      # Pi setup, HiFiBerry
│   └── *.cpp, *.py, *.service
└── deprecated/         # Old/unused code
```

## Quick Start

See `BUILDME.md` for full hardware assembly instructions.

1. Build the automidi controller (Teensy + encoders + displays)
2. Set up autoclip Pi 5 with capture cards
3. Set up autowaaave Pi 3B+ with HiFiBerry
4. Connect automidi to autowaaave via USB (provides both MIDI and serial)
5. Connect autowaaave to autoclip via direct ethernet (10.0.0.1 ↔ 10.0.0.2)
6. Power on all three — system auto-syncs on boot

**Note:** The Teensy has no direct connection to autoclip. All communication flows through autowaaave's video_bridge.py which translates serial↔UDP.

## License

MIT
