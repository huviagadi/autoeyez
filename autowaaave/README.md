# autowaaave

Autowaaave is AutoEyez's openFrameworks renderer and controller bridge.
It receives autoclip video through a GStreamer shared-memory socket,
combines it with GPU feedback history, applies color, geometry, temporal,
and post effects, and outputs HDMI. The renderer is based on Andrei Jay's
[auto_waaave](https://github.com/ex-zee-ex/auto_waaave).

This directory contains source from different generations. Root
`ofApp.cpp` and `autowaaave.service` retain earlier paths. Pi 5 source
and services appear in `../Codex Port/v04/`, `pi5_port/`, and newer staged
directories such as `peek_menu_fix_20261026/`. The Pi 5 renderer uses a
720×480 video input, a 1920×1080 canvas, and a 30-frame feedback ring in
the inspected source. Match the renderer binary, shaders, and receiver
service when deploying.

`video_bridge.py` connects to Teensy USB serial at `/dev/ttyACM0`,
forwards commands to autoclip on UDP 5006, and returns state from UDP 5005.
Renderer parameters and FFT bands also arrive through USB MIDI. Force
offset controls arrive from autoclip on UDP 5010. Optional HiFiBerry
capture-to-HDMI audio runs through `alsaloop.service` and `hdmi_audio.py`.

The raw video receiver listens on TCP 1237. The codec-damage variant in
`../codec_mosh_trial_20261104/` listens through a proxy on TCP 1238 and
feeds a local decoder on TCP 1239. Both routes produce
`/tmp/video_shm` for the renderer. Select one matching sender/receiver pair.

See [the system architecture](../ARCHITECTURE.md) for the full signal flow
and deployment caveats.
