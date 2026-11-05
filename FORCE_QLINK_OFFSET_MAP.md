# Force Program CC offset map

Implemented map for Akai Force Q-Links. The 2026-10-21 update is installed on
the autoclip and autowaaave Pis. The OF app, autoclip mixer, shader controls,
and HDMI volume receiver now accept the rows marked Live. Force Q-Link
assignments themselves still need to be entered on the Force.

## Message and offset rule

- Set a Force MIDI track's output to the autoclip USB MIDI Gadget. Use ordinary
  absolute MIDI CC messages with the full 0–127 value range. No note velocity,
  Program Change, or parameter-select CC is involved.
- Use the same track MIDI channel as the existing Force controls for now.
  Current OF offset receiver ignores the channel. Each parameter has a unique
  CC number, so a second channel is unnecessary for this map. Channel can
  become a bank selector later if the map outgrows the available CCs.
- `63` means no Force offset. `0..62` subtract; `64..127` add. Apply the
  offset after physical knobs, saved patch values, and audio modulation.
  Clamp only where the destination actually has a hard limit; preserve
  existing audio modulation headroom. Do not write the Force value into a
  saved patch. Offset 63 must leave the base value unchanged.
- The CC is converted to a signed, normalized offset: `0 = -1`, `63 = 0`,
  `127 = +1`. Multiply that by **one full sweep of the target parameter's
  physical control**. This is parameter-specific scaling, not a reduced
  trim range. For bloom (CC 25), the physical CC 22 maps 0–127 to 0–1 in the
  renderer, so a Force value of 127 must add `+1.0`: physical bloom 0 plus
  Force 127 reaches the same value as physical bloom 127 when other
  modulation is zero. Likewise Force 0 adds `-1.0`. The sum can exceed 1
  when physical and audio contributions stack, as the current renderer allows.
  This bloom behavior is now live in the OF renderer.
- Force Q-Link changes should be smoothed and held until another value arrives
  or the relevant process restarts. Reset a Q-Link by sending 63.
- CC 22 and 24 control clip, shader, and Peek rune stutter. CC 93 remains audio patch
  load; CC 98 remains input trim; CC 97 remains pixel amount and its F#8 cue.
  E8 (MIDI note 124) provides a held Peek pulse. F8 (MIDI note 125) sets
  the latched Peek base from velocity 1–127 (0–100%); note-off does not reset it.

## OF renderer: feedback, color, geometry, and post effects

| CC | Offset target | Status | Notes |
|---:|---|---|---|
| 16 | Luma key threshold | Live | Existing additive Force layer |
| 17 | Feedback blend | Live | Existing additive Force layer |
| 18 | Hue | Live | Existing additive Force layer |
| 19 | Saturation | Live | Existing additive Force layer |
| 20 | Brightness / value | Live | Existing additive Force layer |
| 21 | Blur / temporal filter mix | Live | Existing additive Force layer |
| 23 | Sharpen | Live | Existing additive Force layer |
| 25 | Bloom / temporal filter resonance | Live | Scalable Teensy CC 22 counterpart |
| 26 | X displacement | Live | Apply in the current X response range |
| 27 | Y displacement | Live | Apply in the current Y response range |
| 28 | Rotation / angle | Live | Apply in the current rotation range |
| 29 | Hue modulo | Live | Current renderer parameter `d_huex_mod` |
| 30 | Hue offset | Live | Renderer meaning; Teensy display labels 125/126 are reversed |
| 31 | Hue LFO | Live | Renderer meaning; Teensy display labels 125/126 are reversed |
| 40 | Feedback delay time | Live | Quantized over one full framebuffer sweep; delay tap wraps in the ring |
| 41 | Fade to black | Live | Scalable Teensy CC 2 counterpart |
| 94 | Mosh amount | Live | Shader residual/vector/refresh values derive from this amount |
| 95 | Chroma amount | Live | Post shader |
| 96 | Crush amount | Live | Post shader |
| 97 | Pixel amount | Live | Post shader; F#8 cue also writes this CC |
| 98 | Line input / FFT gain trim | Live | Existing Force trim; this is not HDMI output volume |
| 99 | Peek visibility | Live | Boost only: 0–63 neutral; 64–127 sweep from saved Peek amount to full visibility |
| 122 | Zoom / Z displacement | Live | Follows selected Teensy zoom range |

Mosh's residual, vector scale, and refresh chance are calculated from the
mosh amount in the live renderer. They are not independently exposed knobs.
This map gives every currently exposed continuous post-effect amount its own
Q-Link address.

CC99 is the exception to the bipolar rule above. The Force sends zero at
some clip boundaries; making zero a negative offset blanked Peek's audio
reactivity. A saved Peek amount stays active at CC99 values 0–63.

## autoclip mixer and output

| CC | Offset target | Status | Notes |
|---:|---|---|---|
| 42 | A/B video blend | Live | Add to the physical `MIX` value; clamp 0–127 |
| 43 | Mixer luma low threshold | Live | Add to physical luma control; keep low ≤ high |
| 44 | Mixer luma high threshold | Live | Add to physical high cutoff; keep high ≥ low |
| 45 | Mixer luma feather | Live | Discrete 0–4 pixel radius after offset |
| 46 | HDMI audio output volume | Live | Independent ALSA softvol after clean capture; 63 = 0 dB, 0 = −50 dB, 127 = +6 dB |

CC 46 changes only the HDMI playback copy. Positive gain above 0 dB can clip
if the incoming signal is already near full scale; keep it at 63 for unity.

Source selection, mirror/invert switches, patch loads, and other discrete
commands do not use bipolar offsets. The HDMI volume reservation is separate
from CC 98, which affects input/FFT trim.

## Shader source slots

These target the three existing source roles, not a specific shader name.
The slot's meaning follows the selected shader. For example, slot 1 might be
wave speed on one shader and orbit speed on another. Role A, Luma, and B each
retain independent patch values. Add the Force offset to the *effective*
value at render time so it stacks with saved base and band reactivity values.

| Role | Slot 1 | Slot 2 | Slot 3 | Status |
|---|---:|---:|---:|---|
| A | CC 50 | CC 51 | CC 52 | Live |
| Luma | CC 53 | CC 54 | CC 55 | Live |
| B | CC 56 | CC 57 | CC 58 | Live |

## Shader audio reactivity offsets

The current shader patch also holds 3 FFT band contributions for each of the
3 slots in each of the 3 roles. These CCs offset those contribution values
without changing the saved audio patch. If a band is silent, its contribution
has no visible effect until that band becomes active.

| Role / band | Slot 1 | Slot 2 | Slot 3 | Status |
|---|---:|---:|---:|---|
| A / Low | CC 60 | CC 61 | CC 62 | Live |
| A / Mid | CC 65 | CC 66 | CC 67 | Live |
| A / High | CC 68 | CC 69 | CC 70 | Live |
| Luma / Low | CC 72 | CC 73 | CC 74 | Live |
| Luma / Mid | CC 75 | CC 76 | CC 77 | Live |
| Luma / High | CC 78 | CC 79 | CC 80 | Live |
| B / Low | CC 81 | CC 82 | CC 83 | Live |
| B / Mid | CC 84 | CC 85 | CC 86 | Live |
| B / High | CC 87 | CC 88 | CC 89 | Live |

CC 63 is deliberately unused because it is the neutral *value* in this Force
offset scheme. CC 64 is left unused because it is the conventional sustain
pedal CC. The offset neutral for the stored shader patch itself is 64; that
stored encoding stays unchanged.

## Reserved and available space

- Existing Force actions: CC 22 and 24 stutter, CC 93 audio patch load, CC 97
  pixel amount/cue, CC 98 input trim. Preserve their behavior.
- CC 0, 32, 64, and 120–127 are avoided for new assignments because they have
  standard MIDI meanings or existing project use. CC 122 is retained because
  it is already the live Force zoom offset.
- CC 47–49, 59, 90–92, and 100–119 remain unassigned in this offset
  map. Check live services before claiming any of them for future controls.
- The map uses 23 renderer addresses, 5 mixer/output addresses,
  9 source slots, and 27 band reactivity addresses.

## Suggested Q-Link pages

The Force project can use multiple MIDI tracks or Q-Link assignment pages to
expose a useful 16-knob subset at a time. These are *suggested layouts*, not
fixed receiver banks: each knob just sends its assigned CC number.

| Page | Q-Links 1–8 | Q-Links 9–16 |
|---|---|---|
| Core | 16, 17, 18, 19, 20, 21, 25, 23 | 94, 95, 96, 97, 26, 27, 28, 122 |
| Sources | 50, 51, 52, 53, 54, 55, 56, 57 | 58, 42, 43, 44, 45, 40, 41, 98 |
| Audio reactivity A | 60, 61, 62, 65, 66, 67, 68, 69 | 70, 29, 30, 31, spare, spare, spare, spare |
| Audio reactivity Luma/B | 72–79 | 80–87; use another page for 88–89 |

**Routing:** OF addresses go to renderer UDP 5010. Autoclip mixer and shader
addresses go through its acknowledged local command socket. CC 46 goes to
the HDMI volume receiver on UDP 5012. The Force forwarder smooths all mapped
CCs. Existing CC 22/24 stutter and CC 93 audio patch behavior remains separate.
The renderer and mixer were restarted after deployment. A live CC 25 packet
was decoded as `offset=1`, the volume receiver was tested at its endpoint and
returned to neutral, and both video processes were observed at 30 fps.
Actual Q-Link assignments and the full range of artistic effects still need
hands-on checking on the Force.
