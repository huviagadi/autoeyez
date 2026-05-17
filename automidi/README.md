# automidi — Teensy MIDI Controller

Hardware MIDI controller for the autoeyez video synthesis system. Features 9 rotary encoders with individual OLED displays, 12 buttons, 8 LEDs, and audio FFT visualization.

## Features

- **9 rotary encoders** with push buttons for parameter control
- **9 OLED displays** (128×64) showing parameter names, values, and graphs
- **3 pages** of parameters (27 total encoder functions)
- **12 buttons** via shift register (menu, controls, functions)
- **8 LEDs** for status indication
- **Audio FFT** via PJRC Audio Shield (32-band spectrum display)
- **32 video patches** + 32 audio patches (save/load to SD)
- **USB MIDI** output to autowaaave
- **Serial** output to autoclip (via autowaaave bridge)

## Communication

| Interface | Target | Data |
|-----------|--------|------|
| USB MIDI | autowaaave | Shader parameters (CC16-127) |
| USB Serial | autowaaave | Clip/mixer commands → bridged to autoclip |

## Pages

### Page 0 — Audio / FX
Encoders: LUMA, BLEND, HUE, SATURATION, X OFFSET, Y OFFSET, SCALE, ANGLE, AUDIO
- Controls shader color and spatial parameters
- Audio encoder adjusts FFT input gain
- Display 8 shows live spectrum analyzer

### Page 1 — Video / Clip
Encoders: VALUE, BLUR, BLOOM, SHARPEN, HUE MODULO, HUE LFO, HUE OFFSET, DELAY TIME
- Controls video feedback and temporal effects
- Encoder 8 scrolls clip list
- Display 8 shows clip player status

### Page 2 — Mixer / Luma Key
Encoders: LUMA LOW, LUMA HIGH, (unused), (unused), LUMA SRC, VIDEO PATCH, AUDIO PATCH, MIXER
- Controls luma keying thresholds and source
- Patch management (save/load)
- Video mixer crossfader and channel selection

## Firmware

Current version: `auto_midi_v0911.ino`

### Dependencies
- Arduino IDE 2.3.8+
- Teensyduino 1.60+
- Adafruit SSD1306 library
- Adafruit GFX library
- Encoder library (Teensy packages version)
- Teensy Audio library

### Upload Notes
Due to cut VUSB trace, bootloader access requires:
1. Power on via 5V rail (VIN pin)
2. Connect USB data cable
3. Hold program button ~20 seconds until dim LED flickers
4. Upload via Teensy Loader

## Files

- `auto_midi_v0911/auto_midi_v0911.ino` — Main firmware
- `BUILDME.md` — Detailed hardware build instructions
- `NOTES.md` — Technical reference notes

## See Also

- `BUILDME.md` for complete wiring diagrams and assembly
- Parent `README.md` for system overview
