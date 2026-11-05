# AutoEyez

AutoEyez is a live video instrument built from a Teensy controller and two
Raspberry Pis. Autoclip plays clips, captures composite video, generates
sources, and mixes the image. Autowaaave applies GPU feedback and effects
and sends the result to HDMI. The controller supplies hands-on parameters,
patches, and audio analysis. See [ARCHITECTURE.md](ARCHITECTURE.md) for the
source-based system map and the distinction between repository variants and
verified device state.

The renderer is based on Andrei Jay's [auto_waaave](https://github.com/ex-zee-ex/auto_waaave).

## Components

| Directory | Purpose |
| --- | --- |
| `automidi/auto_midi_v0911/` | Teensy 4.1 firmware, audio FFT, OLED and encoder UI, SD patches |
| `autoclip/` | Python clip player, composite capture, mixer, generated sources, MIDI gadget |
| `autowaaave/` | openFrameworks renderer source, shaders, serial bridge, audio services |
| `Codex Port/v04/` | Pi 5 renderer and raw shared-memory receiver port |
| `codec_mosh_trial_20261104/` | Annex B H.264 sender, bounded damage proxy, receiver overrides |
| `patch_codec_compat_20261105/` | Patch schema and transport compatibility work |

The source tree also contains dated stages and recovery copies. A stage is
not automatically the installed version. Compare files with each Pi before
deployment.

## Connections

- Teensy USB MIDI and serial connect to autowaaave. MIDI carries renderer
  controls and FFT bands; serial carries clip/mixer commands and state.
- The Pis use direct Ethernet, with source defaults of `10.0.0.1` for
  autowaaave and `10.0.0.2` for autoclip.
- Composite capture dongles and the optional external USB source connect
  to autoclip. Its mixer also plays MP4 files from `/home/admin/clips/`.
- Autowaaave receives the mixed video through GStreamer shared memory,
  renders the final image, and outputs HDMI.
- An Akai Force can connect to autoclip's USB MIDI gadget for cues and
  an additional parameter-offset layer.

## Software setup

This repository is a source and deployment reference, not a single-command
installer. The service examples assume Raspberry Pi OS, Python 3, FFmpeg,
GStreamer, openFrameworks, and the hardware interfaces above. The Python
mixer uses NumPy, OpenCV, and PyAV; generated GPU sources may use ModernGL.
The controller builds with Teensyduino, Teensy Audio, Encoder, Adafruit GFX,
and Adafruit SSD1306.

1. Build and flash the firmware in `automidi/auto_midi_v0911/` for Teensy
   4.1 with USB MIDI and serial enabled.
2. Put clips in `/home/admin/clips/` on autoclip. Configure capture device
   paths in `autoclip/mixer.py` to match the connected USB ports.
3. Install the chosen autoclip mixer/player/control scripts and their
   systemd units. Configure the USB MIDI gadget if using the Force.
4. Build the chosen Pi 5 openFrameworks renderer with its matching shaders.
   Install a matching video receiver and `video_bridge.py`.
5. Select **one** matching stream route: raw YUV420p on TCP 1237, or the
   Annex B codec path through TCP 1238 and local TCP 1239. Check the
   sender and receiver service overrides together.
6. Confirm video in `/tmp/video_shm`, Teensy MIDI and serial, renderer HDMI,
   clip commands, and the chosen audio route.

For the detailed code paths and service names, use [ARCHITECTURE.md](ARCHITECTURE.md).
Older build guides in the tree include Pi 3B+ instructions and should be
checked against the selected Pi 5 source before use.

## Customization

- `autoclip/mixer.py`: source selection, capture paths, mixer behavior,
  stream host and port defaults.
- `autoclip/video_player.py`: clip directory, playback, seek, stutter.
- `autoclip/gpu_shader_sources.py` and `autoclip/queued_shaders/`:
  generated video sources.
- `automidi/auto_midi_v0911/auto_midi_v0911.ino`: controller menus,
  MIDI mappings, page behavior, and audio controls.
- Renderer `ofApp.cpp` and matching `shadersES2/`: feedback and effects.
- `autowaaave/video_bridge.py`: serial/UDP control addresses and ports.
- Service unit files and overrides: process startup and transport route.

Local clip media, compiled binaries, generated previews, and machine-specific
credentials are intentionally excluded from the repository.

## License

MIT
