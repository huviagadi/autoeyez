# autoclip

Autoclip is the video source and mixer Pi in AutoEyez. The current root source
has a PyAV clip player (`video_player.py`), an FFmpeg/V4L2 capture and
composition loop (`mixer.py`), and a UDP command/status bridge
(`video_control.py`). The mixer builds 720×576 RGB frames, crossfades A/B,
optionally overlays a luma-key source, and writes a local framebuffer preview.
It scales the network feed to 720×480.

`video.service` selects the raw YUV420p route on TCP 1237. The staged
`../codec_mosh_trial_20261104/` source and overrides implement an Annex B
H.264 route on TCP 1238. Sender and receiver must use the same route.

Clips live in `/home/admin/clips/`; `video_player.py` prefers matching seek
copies under `/home/admin/clips_seek/`. Capture devices use stable
`/dev/v4l/by-path/` names configured in `mixer.py`. The Akai Force connects
through the MIDI gadget configured by `setup_midi_gadget.sh` and is handled by
`force_cc_forward.py`.

The controller firmware lists 26 source choices, while root `mixer.py`
dispatches only indices 0–15. Staged mixer sources contain later variants;
inspect the installed Pi before changing source menus. `side_keypad.py`
imports modules absent from this root directory, so its deployed module set
must also be checked.

See [the system architecture](../ARCHITECTURE.md) for interfaces, ports, and
the boundary between repository source and live device state.
