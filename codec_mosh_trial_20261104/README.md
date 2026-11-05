# H.264 codec mosh trial, 2026-11-04

Purpose: compare clip cue feel and stable frame rate against the established raw YUV420p path, then test controlled H.264 frame damage offscreen. The live raw path is TCP 1237. All live H.264 trials were temporary and have been rolled back.

## Live A/B results

All cue timings are from a UDP `PLAY:n` command sent on autowaaave to the first large image change in frames read from `/tmp/video_shm` on the same Pi. This measures cue-to-renderer-input, not HDMI glass-to-glass. The probe samples every 256th RGB byte and identifies a visibly distinct cut; it has one-frame (33 ms) resolution.

| Path | Cue-to-input samples | Mixer | Receiver |
|---|---|---|---|
| Raw YUV420p TCP 1237 | 98, 103, 131 ms for alternating clips | 30 fps | `rawvideoparse` |
| H.264 in MPEG-TS TCP 1236 | 232, 235, 300 ms for repeated cues | 30 fps | default `avdec_h264` |
| Bare H.264 TCP 1238 | 199, 199 ms for alternating clips | 30 fps | default `avdec_h264` |
| Bare H.264 TCP 1238, tuned decoder | 100, 105, 132 ms for alternating clips | 30 fps | `avdec_h264 max-threads=1 thread-type=slice`, `shmsink sync=false` |

The last row matched raw cue timing within the 33 ms frame resolution. The tuned receiver used about 13% of one Pi 5 CPU core; the FFmpeg software encoder on autoclip used about 27%. The full mixer still reported 30 fps, p95 work about 15.6 ms, and zero over-budget frames in a five-second sample. These were short tests, not a sustained load or HDMI-output latency study.

The 90-frame representative input encoded at 9.85× real time on autoclip and decoded to RGB at about 5.7 ms/frame wall time on autowaaave when run as an isolated file test. The clean H.264 encode at CRF 23 measured 37.1 dB average PSNR against the raw sample; inspect the image directly before deciding whether this quality is enough.

## Actual coded-frame damage

`source_cut90.h264` contains 45 frames of the live feed followed by 45 frames of a local clip, with an IDR on the cut. `nal_damage.py` parses Annex B NAL units and produces:

- `drop_cut_idr.h264`: removes the cut's whole IDR access unit. The decoder continues with pronounced previous-scene carryover and block motion. See `mosh_cut_preview.mp4` and `mosh_cut_contact.png`.
- `drop_one_p_slice.h264`: removes one predicted slice at frame 52. The decoder continues with a thin horizontal tear. See `slice_loss_preview.mp4` and `slice_loss_contact.png`.

The preview MP4s bake the decoded result so playback does not depend on the viewing app's error concealment. The original damaged H.264 files are also retained. This proves two useful vectors on one sample; other source material and decoders can react differently.

## Live build, 2026-11-04

The bounded codec route is now live. Autoclip runs `/home/admin/mixer_codec.py` and emits Annex B H.264 with one AUD per frame to TCP 1238. Autowaaave runs `/home/admin/realtime_codec_damage.py`, which forwards NALs to the local GStreamer receiver on 1239. The receiver uses the single-threaded, slice decoder configuration measured above. The controller firmware was built from the exact source that reproduced the saved pre-Angel hex hash. The current H.264 service configuration persists across reboot.

The first two Processing rows are `AUDIO CODEC` and `STATIC CODEC`. `AUDIO CODEC` is the audio master; `STATIC CODEC` independently sets the fraction of predicted frames damaged and increases the number of slices removed per selected frame, from none at 0 to five slices on every predicted frame at 127. Slice positions rotate over time. `CD LOW`, `CD MID`, `CD HIGH`, and `CD RATE` set the audio band gains and onset rate. The proxy subscribes to the existing Teensy MIDI FFT CCs through ALSA. Bass uses the extended 14-bit value because its coarse CC can saturate at 127. Controls are sent over the existing Teensy serial bridge to `/dev/shm/codec_damage` and save in version 7 video patches; version 5 and 6 patches load with Static Codec at zero. The old MOSH shader receives CC94=0 on controller startup and patch recall.

Impulse update: all bands require a rise above a moving baseline, then stay disarmed until the signal falls for three frames. Low makes one slice tear per hit; only high `CD LOW` settings can additionally suppress a nearby IDR, with a two-second cap. Mid makes a longer alternating-slice burst across three to five predicted frames. High makes a shorter two to four frame burst; `CD HIGH` at 96 or above removes more slices per frame. `CD RATE` sets the per-band onset cooldown, while release gating keeps held notes from repeatedly firing. The previous proxy remains backed up as `/home/admin/realtime_codec_damage.pre_impulse_20261104.py` on autowaaave.

The clean proxy path reached the renderer input in 68–135 ms across sequential alternate clip cues; two further cues on the final AUD-enabled configuration measured 132 and 125 ms. These fall within one 33 ms frame of the raw baseline (98–131 ms). The mixer stayed at 30 fps. Four synthetic MIDI band hits addressed only to the proxy produced damage counters of low=3, mid=1, high=1. The stream kept decoding and the mixer stayed at 30 fps afterward. These measurements stop at renderer input, not HDMI glass-to-glass. The actual visual effect depends on source motion and decoder concealment.

For quick inspection on autowaaave: `cat /dev/shm/codec_damage` shows audio, low, mid, high, rate, static; `cat /dev/shm/codec_damage_stats` shows frame count followed by low/mid/high/static counts for the current sender connection. Run `./restore_raw_stream.sh` on the Mac to remove the persistent codec service overrides and restore the original raw wire path. The prior firmware hex, bridge, and patch validator remain backed up on autowaaave with `pre_codec_20261104` and `pre_static_20261104` in their names.

The 2026-11-04 Static Codec update also lowered audio onset thresholds slightly across all three bands, with the largest change on mid. The held-note regression still produced one onset per band until a release and retrigger. In a 90-frame offline stream, static values 0, 64, and 127 damaged 0, 44, and 88 predicted frames respectively; all three files decoded without FFmpeg errors. On the live device, a brief Static Codec value of 64 advanced the static damage counter while the mixer held 30 FPS. It was returned to zero, and the prior audio controls (127, 88, 66, 127, 42) were restored after the controller flash. This is a deterministic duty cycle, so the percent describes how often a frame is broken, not a literal percent of its pixels.

Follow-up: the proxy's 14-bit bass FFT value had been divided by 128, making it 32 times smaller than the coarse 0–127 band scale. The live proxy now divides by 4. A read-only eight-second shadow run on the live FFT feed found eight bass onsets after this correction. The corrected proxy was installed as `/home/admin/realtime_codec_damage.py` with its predecessor saved as `realtime_codec_damage.pre_bass_scale_20261104.py`.

MIDI reconnect fix: after the Teensy flash, ALSA left the proxy's input open but disconnected, so video frames continued while all audio-trigger counters froze. The proxy now reopens its MIDI input when the Teensy's FFT frame heartbeat is absent for one second. A live test deliberately removed the ALSA subscription; the proxy reconnected automatically and the mid/high hit counters resumed. The prior proxy is saved as `realtime_codec_damage.pre_midi_watchdog_20261104.py`.

Static follow-up: the original rotation formula always picked slice 2, and its counter recorded selected frames even without a removed slice. The new version rotates across slices 2–6, increases the number removed as the control rises, and counts only frames where a slice was actually removed. In the 90-frame offline sample, settings 0/64/127 damaged 0/44/88 predicted frames and all 90 frames decoded. With the live control at 127, the counter advanced from 119 to 207 in three seconds and the mixer reported 30 FPS. The prior proxy is saved as `realtime_codec_damage.pre_static_spread_20261104.py`.

Persistent static test: suppressing complete keyframes avoided clean refreshes but caused a recurring gap in playback. The revised proxy forwards the first IDR intact and removes one rotating slice from later IDRs while Static Codec is above zero, keeping a decodable frame and carrying some damage through the refresh. At zero the next keyframe is clean. Audio-triggered IDR suppression remains unchanged when Static Codec is off. In a 270-frame offline repeated-stream test, each later IDR retained six of seven slices and all 270 frames decoded without reported errors. Maximum predicted-frame damage was reduced from five slices to three. The complete-keyframe version is backed up as `realtime_codec_damage.pre_partial_idr_20261104.py`.

Gradual refresh update: partial IDRs still produced a visible periodic flash. The live autoclip H.264 encoder now uses x264 `intra-refresh=1:scenecut=0` with a 30-frame refresh interval, so the picture refreshes progressively rather than receiving a full IDR every second. This reduces abrupt resets but also means the audio bass effect's occasional IDR suppression has no recurring IDR to suppress; its predicted-slice hit remains active. The decoder reconnect receives a fresh startup IDR because the sender restarts its encoder on reconnect. A 90-frame offline test with static damage for 45 frames decoded all 90 frames without reported errors. The live mixer state and clip were restored after the restart; the mixer reported 30 FPS. The previous encoder configuration is backed up as `/home/admin/mixer_codec.pre_intra_refresh_20261104.py` on autoclip.

2026-11-05 ceiling increase: static can now remove up to five slices per selected predicted frame at maximum. Above 112/127 on both Audio Codec and an individual band control, the band's hits remove more slices and last longer; normal onset and release gating still apply. An offline 90-frame intra-refresh stream decoded all frames without FFmpeg errors for maximum static and each separate audio band. A held input generated one hit per band. The mixer reported 30 FPS after deployment; audio counters were idle during the brief post-deploy check. The previous proxy is backed up as `/home/admin/realtime_codec_damage.pre_stronger_20261105.py`.

The `FPS: 12.5` line in the autoclip journal can be the selected clip's decode rate: `024_BOISVERT_FALL.mp4` reports a `frame_period` of 0.08 seconds. Use `MIXER_PERF.fps` and the proxy frame counter to judge output throughput; both remained near 30 during this check.

At the user's 127/78/127/127/127 settings, the old proxy had counted 117 low, 112 mid, and 7 high hits: bass was too repetitive and high was starved. An eight-second MIDI sample showed low extended median 12.3, mid median 11, high median 2. A read-only shadow run of the revised onset detector over ten seconds counted 5 low, 12 mid, and 24 high onsets. The live updated stream kept `MIXER_PERF.fps` at 30 with zero over-budget frames. Offline 90-frame decode tests of the stronger mid and high bursts produced no decoder errors and visibly changed the decoded frames.

Temporary systemd drop-ins used for the live A/B trials:

- autoclip: `/run/systemd/system/video.service.d/90-mosh-trial.conf`
- autowaaave: `/run/systemd/system/stream_receive_shm.service.d/90-mosh-trial.conf`

Both drop-ins were installed in `/run`, then removed. To roll back a future test manually, remove both files, run `sudo systemctl daemon-reload`, restart `stream_receive_shm.service` on autowaaave and `video.service` on autoclip. Start the receiver before the sender. Each live test also used a five-minute timed rollback unit.

At test start, record current clip index, playback position, mixer state, and baseline mixer FPS. Compare those and renderer FPS after the switch. If the output degrades or a cue hangs, roll back immediately. This trial restored clip 23 near 54.64 seconds and all captured mixer values.
