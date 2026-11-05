# autoeyez MIDI CC reference

**Current Force Q-Link offset map (2026-10-21):** See
`FORCE_QLINK_OFFSET_MAP.md`. The Force table below is the older partial map;
the new file covers all 63 active scalable addresses, including autoclip
shader and mixer controls plus HDMI output volume.

Current v04 renderer and autoclip Force forwarding path. Verified against
`Codex Port/v04/ofApp.cpp`, `autoclip/force_cc_forward.py`, and
`automidi/auto_midi_v0911/auto_midi_v0911.ino` on 2026-09-17.

## Akai Force Q-Link CCs

The Force connects to autoclip as `f_midi`. Its CC messages are forwarded to
autowaaave on UDP 5010. **Only the CCs in this table affect the Force offset
layer.** MIDI channel does not matter. This layer adds to the current patch
and audio result; it never changes saved patch values.

**Value 63 = zero offset.** Values 0–62 subtract; 64–127 add. Fast smoothing
reaches about 90% of a jump in 0.10 seconds. Sending 63 restores the patch
value. An offset holds until another value arrives or the renderer restarts.
The final amount of each post effect
is limited to 0–1, so a negative offset cannot make a zero-base effect run
below zero.

| CC | Force controls | Offset at 0 / 127 |
|---:|---|---:|
| 16 | Luma key | −1 / +1 |
| 17 | Feedback blend | −2 / +2 |
| 18 | Hue | −1 / +1 |
| 19 | Saturation | −1 / +1 |
| 20 | Brightness / value | −1 / +1 |
| 21 | Blur / temporal filter mix | −1 / +1 |
| 23 | Sharpen | −1 / +1 |
| 94 | Mosh | −1 / +1 |
| 95 | Chroma | −1 / +1 |
| 96 | Crush | −1 / +1 |
| 97 | Pixel | −1 / +1 |
| 99 | Peek visibility | 0 / +1 (0–63 neutral) |
| 122 | Zoom / Z displacement | −/+ current zoom range × renderer zoom scale |

For Peek, Q-Link CC99 sweeps from the saved Processing Peek amount to full
visibility over values 64–127; 0–63 leave the saved amount unchanged. E8
(MIDI note 124) adds a second positive offset set by note
velocity: 127 adds +1. The offset holds while the note is held and decays
after note-off. The combined amount is clamped to 0–1 and still follows the
low/mid/high audio hit gates in the Peek shader. Neither MIDI layer changes
the saved patch value.

CC 120–121, CC 123–127, toggles, and patch commands are **not** mapped
in the Force visual layer. The forwarder transmits them, but v04 ignores them on UDP 5010.
This separation keeps Force cues from changing the Teensy's patch or mode
state. The old autoclip CC 1 seek service is currently inactive.

CC 22 controls autoclip stutter. Value 0 releases it. Values 1–127 capture
the current clip position when first engaged and map linearly to a 2–30 frame
forward loop (about 67 ms to 1 second). Changing the nonzero value changes the
length while retaining the captured start point.

CC 24 controls the same autoclip stutter with its length reversed for Force
momentary controls. Value 0 releases it; value 1 selects 30 frames and value
127 selects 2 frames. Intermediate values map linearly in reverse.

Both CC22 and CC24 also loop the frame order of Peek's bass, mid, and high
rune decks on autowaaave using the same video-frame lengths. The rune image
visible when stutter starts is the loop anchor; releasing both controls
returns the decks to audio-hit stepping.

Both controls share one captured buffer but have independent loop heads. Each
works alone as described. When both are nonzero, their two frame positions are
blended 50/50. Releasing one leaves the other running; releasing both resumes
forward playback from the buffered loop.

Force CC 122 uses the zoom range selected by Teensy CC 34/50/66
(1x/2x/5x/10x). With no range button active it uses 1x. Its final scale is
kept positive even at the largest negative offset.

## Teensy / automidi continuous controls

These CCs arrive through the renderer's USB MIDI input. Unlike the Force
layer, they control the renderer's normal parameter or audio assignment.
Several controls change meaning when low, mid, or high audio assignment mode
is selected.

| CC | Control | Teensy label / detail |
|---:|---|---|
| 2 | Fade to black | Global blackout amount |
| 7 | Audio input gain | AUDIO encoder; maps 0–127 to gain 0.1–4 |
| 16 | Luma key threshold | LUMA |
| 17 | Feedback mix | BLEND |
| 18 | Hue | HUE |
| 19 | Saturation | SATURATION |
| 20 | Brightness / value | VALUE |
| 21 | Temporal filter mix | BLUR |
| 22 | Temporal filter resonance | BLOOM |
| 23 | Sharpen | SHARPEN |
| 94 | Motion smear / datamosh | MOSH; 0 bypasses |
| 95 | Chromatic aberration | CHROMA; 0 bypasses |
| 96 | Color depth reduction | CRUSH; 0 bypasses |
| 97 | Pixelation | PIXEL; 0 bypasses |
| 102 | Input chroma bleed | BLEED on Page 3 PROCESSING; 0 bypasses |
| 103 | Input chroma misalignment | ALIGN on Page 3 PROCESSING; 0 bypasses |
| 104 | Input analog ghosting | GHOST on Page 3 PROCESSING; 0 bypasses |
| 105 | Input time-base wobble | WOBBLE on Page 3 PROCESSING; 0 bypasses |
| 106 | Input overdriven composite | OVERDRIVE on Page 3 PROCESSING; 0 bypasses |
| 120 | Horizontal displacement | X OFFSET |
| 121 | Vertical displacement | Y OFFSET |
| 122 | Zoom / Z displacement | SCALE |
| 123 | Rotation | ANGLE |
| 124 | Hue modulo | HUE MODULO |
| 125 | Hue offset | Renderer mapping; Teensy display currently says HUE LFO |
| 126 | Hue LFO | Renderer mapping; Teensy display currently says HUE OFFSET |
| 127 | Feedback delay time | DELAY TIME |

The CC 125 and 126 display labels are reversed relative to the renderer code.
Use the renderer mapping in the table when assigning external controls.

## Teensy / automidi mode and switch CCs

For these switches, 127 generally turns the setting on and 0 turns it off.

| CC | Control |
|---:|---|
| 39 | Video-reactive control mode |
| 42 | Assign next parameter movement to high audio band |
| 43 | Assign next parameter movement to low audio band |
| 44 | Assign next parameter movement to mid audio band |
| 41 | Horizontal mirror |
| 45 | Vertical mirror |
| 46 | Toroidal wrap |
| 59 | Clear live video/audio modulation assignments |
| 60 | Invert luma; also sets brightness invert in current code |
| 61 | Invert saturation; also sets mirror mode in current code |
| 62 | Invert hue |
| 71 | Wet/dry feedback mode |
| 90 | Aspect-ratio switch, currently overridden to off every draw frame |
| 91 | Patch-load latch; legacy pickup/p-lock support |
| 92 | Save audio patch; CC value 1–64 is patch slot |
| 93 | Load audio patch; CC value 1–64 is patch slot |
| 58 | Clear feedback image/history |

Force piano-roll patch recalls use note-on velocity as the slot number:

| Force note | MIDI note | Velocity | Action |
|---|---:|---:|---|
| C#8 | 121 | 1–64 | Load audio patch AUDIO01–AUDIO64 via CC 93 |
| D8 | 122 | 1–64 | Load video patch PATCH01–PATCH64 via the serial bridge |

Note-off does not recall another patch. Empty slots are rejected by their loader.

CC 60 and 61 each trigger two handlers in `midibiz()`. This is the behavior
of the current source, not two independent assignments. P-lock is deprecated
for this device; CC 55 (p-lock master) and the p-lock action of CC 91 remain
in legacy code but are not part of the intended audio/MIDI workflow. CC 59
does not clear Force offsets; send 63 for each assigned Force CC instead.

## Teensy range selection CCs

These are button states used to choose a parameter's response range. They
do not directly set the parameter value.

| Parameter | Range selector CCs |
|---|---|
| X displacement | 32, 48, 64 |
| Y displacement | 33, 49, 65 |
| Zoom / Z displacement | 34, 50, 66 |
| Rotation | 35, 51, 67 |
| Hue modulo | 36, 52, 68 |
| Hue offset | 37, 53, 69 |
| Hue LFO | 38, 54, 70 |

Clip selection, playback, mixer channels, and luma source controls travel as
serial/UDP text commands, not as MIDI CCs to the renderer. The Teensy's VIDEO
encoder sends CC 0, but the v04 renderer has no CC 0 parameter handler.
