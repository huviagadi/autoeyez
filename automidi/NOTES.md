# automidi — Notes

## Hardware
- **Teensy 4.1** @ 600MHz, 1MB RAM, 8MB flash
- VUSB trace cut — powered via 5V on VIN pin; USB used for MIDI data only
- AMS1117-3.3 regulator (from VUSB 5V) supplies 3.3V to all OLEDs and shift registers independently (onboard Teensy regulator failed once from overload)
- **PJRC Audio Shield Rev D** (SGTL5000) stacked on Teensy — line-in via solderable pads; AudioControlSGTL5000 must be declared AFTER display array or Teensy hangs on boot
- 9 rotary encoders with pushbutton (pins 30–47, CLK/DT pairs) — Paul Stoffregen Encoder library
- 12 buttons via two daisy-chained **74HC165** shift registers (DATA=26, CLK=16, LOAD=17); 10kΩ external pullups required on all inputs (no internal pullups)
- 8 LEDs via **74HC595** shift register (DATA=22, CLK=24, LATCH=25)
- 9 × 128×64 **SH1106** OLEDs (vendor-listed as SSD1306, driven with SSD1306 library); SPI bus: SCK=13, MOSI=11, DC=15, RES=29; CS pins 0–6, 9, 14; 33Ω series resistors on SCK/MOSI/DC critical for signal integrity
- SD card (CS=10) — patch storage

## Bootloader Access
Requires: power on via 5V rail first, then connect USB while holding program button ~20 seconds until dim flickering LED appears (non-trivial due to cut VUSB trace)

## Loop Timing Budget
- Normal loop: ~345µs
- With one display update: ~1600µs
- Hard limit: ~2900µs (audio interrupt starvation above this)
- Rule: maximum ONE display update per loop iteration

## Pages

### Page 0 — Audio / FX
- 9 encoders: LUMA, BLEND, HUE, SATURATION, X OFFSET, Y OFFSET, SCALE, ANGLE, AUDIO
- BTN_1–4: toggle INVERT LUMA, TOROIDAL, INVERT HUE, INVERT SAT
- BTN_5–8: encoder speed multiplier (0×/2×/5×/10×) for enc0–3
- Ring buttons: select audio-reactive band (low/mid/hi) → LEDs 6/7/8
- Display 8: LINE IN + live 32-band FFT with peak hold

### Page 1 — Video / Clip
- 8 encoders: VALUE, BLUR, BLOOM, SHARPEN, HUE MODULO, HUE LFO, HUE OFFSET, DELAY TIME
- BTN_1–4: INVERT SAT, H MIRROR, V MIRROR, WET/DRY
- Ring buttons: LOW=loop toggle, MID=play/pause, HI=next clip
- Enc8: scrolls clip list
- MENU short press: play or load selected clip
- Display 8: VIDEO OUT — clip name, play/pause, loop, progress bar, clip list (3 visible)

### Page 2 — Mixer / Luma Key
- Enc0: luma key low threshold
- Enc1: luma high threshold
- Enc4: luma source selection (scroll)
- Enc8: crossfader (mix view) or channel A/B source scroll
- Ring: LOW=Ch A select, MID=Mix crossfader, HI=Ch B select
- BTN_1/2: toggle luma low/high cut on/off
- BTN_5: confirm luma source
- Enc5 + BTN_6: tap=load video patch, hold=save video patch
- Enc6 + BTN_7: tap=load audio patch, hold=save audio patch
- Displays: LUMA KEY, LUMA HI, LUMA SRC, VIDEO PATCHES, AUDIO PATCHES, VIDEO MIXER

## Communication

### Serial → autoclip (115200 baud, USB serial)
- Sends: PLAY, PAUSE, LOOP_ON, LOOP_OFF, NEXT, PLAY:n, MIX:n, CH_A:n, CH_B:n, LUMA:n, LUMA_HIGH:n, LUMA_HIGH_EN:n, LUMA_SRC:n
- Receives: START, FILE:name, PAUSE:n, LOOP:n, POS:n:total, LIST:name1,..., PROGRESS:pos:total
- Single 'R' → Teensy reboot

### USB MIDI → autowaaave
- Enc CCs page 0: CC16–23 (enc0–7), CC7 (audio/enc8)
- Enc CCs page 1: CC120–127
- Control CCs: 41, 45, 46, 59, 60, 61, 62, 71
- Reactive band CCs: CC42 (hi), CC43 (low), CC44 (mid)
- CC91: latch signal — high before patch CC sweep, low after (autowaaave batches apply)
- CC92: save audio patch (value = slot+1)
- CC93: load audio patch (value = slot+1)

## Patch System
- Video patches: PATCH01.TXT–PATCH32.TXT on SD
  - Stores: 18 enc values, 8 controls, 8 functions, luma settings, mix, channel sources, clip pos, loop, shader/color params
  - On load: restores local state + queues serial commands to autoclip + replays MIDI CC sweep with CC91 latch
- Audio patches: AUDIO01.TXT–AUDIO32.TXT — marker files only; actual data lives on autowaaave
  - Save: sends CC92 + slot, writes marker file to SD
  - Load: sends CC93 + slot

## Boot Sequence
- OLEDs display: A · U · T · O · E E E · Y Y Y · E E E · Z Z Z · <3 &
- LED scroll while waiting for autowaaave `START` message (timeout: 80 seconds)
- On ready: init all MIDI CCs to defaults → full sweep+latch to sync autowaaave

## Draw System
- Non-blocking: one display update per loop iteration
- Dirty flags per display — priority: luma/mixer → labels → controls → functions → graphs

## Known Issues
- Patch load updates UI correctly but does NOT resend MIDI CCs to autowaaave — parameters display correctly on screens but autowaaave is not updated
- Audio crashes on rapid page changes (interrupt starvation from burst of display updates)
- SH1106 garbage pixels on right edge of all displays (132-column RAM, 128 shown)
- AudioControlSGTL5000 declaration order sensitive — must follow display array declaration
- Keep only Teensyduino packages version of Encoder library (two copies found during dev)

## Development Environment
- Arduino IDE 2.3.8 / Teensyduino 1.60
- Libraries: Adafruit SSD1306, Adafruit GFX, Encoder (Teensy packages), Teensy Audio
- Serial monitor: 115200 baud

## Current Firmware
- `auto_midi_v0911.ino`
