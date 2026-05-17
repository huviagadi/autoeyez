# automidi — Hardware Build Guide

Detailed assembly instructions for the Teensy-based MIDI controller.

## Bill of Materials

| Qty | Part | Specification | Notes |
|-----|------|---------------|-------|
| 1 | Teensy 4.1 | 600MHz, 1MB RAM, 8MB flash | |
| 1 | PJRC Audio Shield Rev D | SGTL5000 codec | Solder line-in pads |
| 9 | Rotary encoder | 24 pulse, with pushbutton | 5-pin (A, B, SW, VCC, GND) |
| 9 | SH1106 OLED 128×64 | SPI interface | Often mislabeled SSD1306 |
| 2 | 74HC165 shift register | 8-bit parallel-in serial-out | Button inputs |
| 1 | 74HC595 shift register | 8-bit serial-in parallel-out | LED outputs |
| 8 | LED with internal resistor | 5-12V rated | Button indicators |
| 3 | Pushbutton | Momentary, normally open | Menu/standalone |
| 1 | AMS1117-3.3V regulator | SOT-223 or TO-220 | External 3.3V rail |
| 12 | 10kΩ resistor | 1/4W | 74HC165 pullups |
| 3 | 33Ω resistor | 1/4W | SPI signal integrity |
| 9 | 100nF capacitor | Ceramic | OLED decoupling |
| 1 | SD card | Any size | Patch storage |
| 1 | 5V power supply | 1A+ | External power via VIN |

## Critical Build Notes

### VUSB Trace Cut
The Teensy VUSB trace must be cut so the board is powered externally via the VIN pin. USB provides data only (MIDI + serial). This prevents power conflicts and allows proper boot sequencing.

**Location:** Small trace on bottom of Teensy between VIN and VUSB pads — cut with exacto knife.

### External 3.3V Regulator
The Teensy's onboard 3.3V regulator cannot handle the current draw of 9 OLEDs + 3 shift registers + LEDs (~265mA). An external AMS1117-3.3V regulator is required.

**Wiring:**
- AMS1117 VIN → 5V power rail (from VUSB pad or VIN)
- AMS1117 VOUT → 3.3V rail for all displays and shift registers
- AMS1117 GND → common ground

### SPI Signal Resistors
33Ω series resistors on SCK, MOSI, and DC lines are **critical** for signal integrity. Without them, displays show jitter and garbage pixels.

## Pin Assignments

### SPI Bus (Shared by all OLEDs)
| Signal | Teensy Pin | Notes |
|--------|------------|-------|
| SCK | 13 | 33Ω series resistor |
| MOSI | 11 | 33Ω series resistor |
| DC | 15 | 33Ω series resistor |
| RES | 29 | Directly connected (no resistor) |

### OLED Chip Select Pins
| Display | CS Pin | Function |
|---------|--------|----------|
| 0 | 0 | Encoder 0 display |
| 1 | 1 | Encoder 1 display |
| 2 | 2 | Encoder 2 display |
| 3 | 3 | Encoder 3 display |
| 4 | 4 | Encoder 4 display |
| 5 | 5 | Encoder 5 display |
| 6 | 6 | Encoder 6 display |
| 7 | 9 | Encoder 7 display |
| 8 | 14 | Encoder 8 display (FFT/Status) |

### Rotary Encoders
| Encoder | CLK Pin | DT Pin | Notes |
|---------|---------|--------|-------|
| 0 | 30 | 31 | |
| 1 | 32 | 33 | |
| 2 | 34 | 35 | |
| 3 | 36 | 37 | |
| 4 | 38 | 39 | |
| 5 | 40 | 41 | |
| 6 | 42 | 43 | |
| 7 | 44 | 45 | |
| 8 | 46 | 47 | |

Encoder pushbuttons are read via 74HC165 shift registers, not directly.

### 74HC165 Button Shift Registers (Daisy-chained)
| Signal | Teensy Pin |
|--------|------------|
| DATA (QH of chip 2) | 26 |
| CLK | 16 |
| LOAD | 17 |

**Chip 1 inputs (directly connected):**
| Bit | Input |
|-----|-------|
| 0 | Standalone button 1 |
| 1 | Standalone button 2 |
| 2 | Standalone button 3 |
| 3 | Encoder 0 switch |
| 4 | Encoder 1 switch |
| 5 | Encoder 2 switch |
| 6 | Encoder 3 switch |
| 7 | Encoder 4 switch |

**Chip 2 inputs:**
| Bit | Input |
|-----|-------|
| 8 | Encoder 5 switch |
| 9 | Encoder 6 switch |
| 10 | Encoder 7 switch |
| 11 | Encoder 8 switch |
| 12-15 | Unused (tie to 3.3V) |

**Important:** All inputs require external 10kΩ pullup resistors to 3.3V — the 74HC165 has no internal pullups.

**Daisy chain:** Chip 1 QH (pin 9) → Chip 2 SER (pin 10)

### 74HC595 LED Shift Register
| Signal | Teensy Pin |
|--------|------------|
| DATA (SER) | 22 |
| CLK (SRCLK) | 24 |
| LATCH (RCLK) | 25 |

**Output mapping:**
| Bit | LED |
|-----|-----|
| 0 | Button LED 1 |
| 1 | Button LED 2 |
| 2 | Button LED 3 |
| 3 | Standalone LED 1 |
| 4 | Standalone LED 2 |
| 5 | Standalone LED 3 |
| 6 | Standalone LED 4 |
| 7 | Standalone LED 5 |

### Audio Shield
The PJRC Audio Shield Rev D stacks on the Teensy headers. Solder the line-in pads on the shield for audio input.

**Reserved pins (do not use):**
- 7, 8, 20, 21, 23 (I2S audio)
- 18, 19 (I2C for SGTL5000 control)

### SD Card
| Signal | Teensy Pin |
|--------|------------|
| CS | 10 (built-in) |

Uses Teensy's built-in SD card slot.

## OLED Wiring

Each OLED module has 7 pins:

| Pin | Connect To |
|-----|------------|
| GND | Ground |
| VCC | 3.3V (external regulator) |
| SCK | Pin 13 (via 33Ω) |
| MOSI | Pin 11 (via 33Ω) |
| RES | Pin 29 |
| DC | Pin 15 (via 33Ω) |
| CS | Individual CS pin (see table above) |

**Decoupling:** 100nF ceramic capacitor from VCC to GND on each OLED, as close to the module as possible.

## Assembly Order

1. **Cut VUSB trace** on Teensy (required before anything else)
2. **Solder headers** to Teensy and Audio Shield
3. **Build external 3.3V regulator** circuit
4. **Wire SPI bus** with 33Ω resistors on SCK, MOSI, DC
5. **Connect OLEDs** one at a time, test each
6. **Wire rotary encoders** to Teensy pins
7. **Build 74HC165 daisy chain** with pullup resistors
8. **Build 74HC595 LED driver** circuit
9. **Stack Audio Shield** on Teensy
10. **Connect 5V power** to VIN
11. **Upload firmware** via USB (see bootloader notes)

## Bootloader Access

With VUSB cut, entering bootloader mode requires:

1. Connect 5V power to VIN pin
2. Connect USB data cable to computer
3. Press and hold the program button on Teensy
4. Wait ~20 seconds for dim, flickering LED
5. Release button
6. Upload via Teensy Loader application

## Troubleshooting

| Problem | Likely Cause | Solution |
|---------|--------------|----------|
| Display garbage/jitter | Missing SPI resistors | Add 33Ω on SCK, MOSI, DC |
| Displays don't initialize | Wrong RES handling | Check RES pin wiring |
| Buttons read erratically | Missing pullups | Add 10kΩ to 3.3V on all inputs |
| Teensy hangs on boot | Audio declaration order | Ensure SGTL5000 declared after displays |
| LEDs flicker on boot | Normal | 74HC595 floats until initialized |
| Can't enter bootloader | VUSB not cut properly | Verify trace is fully cut |
| Overheating | Onboard regulator overload | Add external 3.3V regulator |

## Power Budget

| Component | Current (typical) |
|-----------|-------------------|
| 9× OLED displays | ~180mA (20mA each) |
| 3× shift registers | ~15mA |
| 8× LEDs | ~40mA |
| Teensy 4.1 | ~100mA |
| Audio Shield | ~30mA |
| **Total** | **~365mA @ 3.3V** |

The AMS1117-3.3 is rated for 1A, providing adequate headroom.

## Enclosure

Panel cutouts needed:
- 9× encoder shafts (typically 7mm diameter)
- 9× OLED windows (roughly 30×15mm visible area)
- 3× standalone buttons
- 8× LED indicators
- USB port access (side)
- Power jack (side)
