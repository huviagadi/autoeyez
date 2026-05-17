# autoeyez — Hardware Build Overview

Complete bill of materials and assembly overview for the three-component video synthesis system.

## System Components

| Component | Platform | Role |
|-----------|----------|------|
| automidi | Teensy 4.1 | MIDI controller with encoders, OLEDs, buttons |
| autoclip | Raspberry Pi 5 | Clip player, composite capture, video mixer |
| autowaaave | Raspberry Pi 3B+ | GPU shader video processor |

## Full Bill of Materials

### automidi Controller
| Qty | Part | Notes |
|-----|------|-------|
| 1 | Teensy 4.1 | 600MHz ARM Cortex-M7 |
| 1 | PJRC Audio Shield Rev D | SGTL5000 codec, line in via solder pads |
| 9 | Rotary encoder with pushbutton | Standard 24-pulse, 5-pin |
| 9 | SH1106 128×64 OLED (SPI) | Often mislabeled as SSD1306 |
| 2 | 74HC165 shift register | Button inputs (daisy-chained) |
| 1 | 74HC595 shift register | LED outputs |
| 8 | 5-12V LEDs with internal resistor | For button indicators |
| 3 | Standalone buttons | Menu, function buttons |
| 1 | AMS1117-3.3V regulator | External 3.3V for displays/ICs |
| 12 | 10kΩ resistors | Pullups for 74HC165 inputs |
| 3 | 33Ω resistors | Series on SPI lines (signal integrity) |
| 9 | 100nF ceramic capacitors | Decoupling on each OLED |
| 1 | SD card | Patch storage |
| 1 | 5V power supply | Powers Teensy via VIN (VUSB cut) |

**See `automidi/BUILDME.md` for detailed wiring and assembly.**

### autoclip (Pi 5)
| Qty | Part | Notes |
|-----|------|-------|
| 1 | Raspberry Pi 5 | 4GB+ RAM recommended |
| 3 | USB composite capture card | MJPEG 640×480 capable |
| 1 | USB hub (powered) | For 3 capture cards |
| 1 | microSD card 32GB+ | OS + clip storage |
| 1 | Ethernet cable | Direct to autowaaave |
| 1 | 5V 5A USB-C power supply | |

**See `autoclip/BUILDME.md` for capture card details and setup.**

### autowaaave (Pi 3B+)
| Qty | Part | Notes |
|-----|------|-------|
| 1 | Raspberry Pi 3B+ | |
| 1 | HiFiBerry DAC+ ADC | Line in for FFT audio |
| 1 | microSD card 16GB+ | OS + openFrameworks |
| 1 | Ethernet cable | Direct to autoclip |
| 1 | HDMI cable | Output to display/mixer |
| 1 | 5V 3A power supply | |

**See `autowaaave/BUILDME.md` for Pi setup and configuration.**

## Network Configuration

The two Pis communicate directly via ethernet (no router needed):

| Device | IP Address | Hostname |
|--------|------------|----------|
| autowaaave | 10.0.0.1 | autowaaave.local |
| autoclip | 10.0.0.2 | autoclip.local |

Configure static IPs in `/etc/dhcpcd.conf` on each Pi:
```
interface eth0
static ip_address=10.0.0.X/24
```

## Connections Overview

```
┌──────────────────────────────────────────────────────────────┐
│                        automidi                              │
│  ┌─────────┐                                                 │
│  │Teensy   │──USB──→ autowaaave (MIDI)                       │
│  │4.1      │──Serial→ video_bridge.py → UDP → autoclip       │
│  └─────────┘                                                 │
│  ↑ 5V power (separate supply, VUSB trace cut)                │
└──────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────┐
│                       autowaaave                             │
│  ┌─────────┐                                                 │
│  │Pi 3B+   │←─USB─── Teensy (MIDI + serial)                  │
│  │HiFiBerry│←─Line In─── Audio source (for FFT)              │
│  │         │──HDMI──→ Display / video mixer                  │
│  │         │←─Ethernet─→ autoclip (10.0.0.1 ↔ 10.0.0.2)      │
│  └─────────┘                                                 │
└──────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────┐
│                        autoclip                              │
│  ┌─────────┐                                                 │
│  │Pi 5     │←─USB─── 3× composite capture cards              │
│  │         │──TCP:1236──→ autowaaave (video stream)          │
│  │         │←─Ethernet─→ autowaaave (10.0.0.2 ↔ 10.0.0.1)    │
│  │         │──Composite/HDMI──→ (optional local monitor)     │
│  └─────────┘                                                 │
└──────────────────────────────────────────────────────────────┘
```

## Power Requirements

| Device | Voltage | Current | Notes |
|--------|---------|---------|-------|
| automidi | 5V | ~500mA | Via VIN pin (VUSB cut) |
| autoclip | 5V | 5A | USB-C, powers capture cards via hub |
| autowaaave | 5V | 3A | Micro-USB |

**Total system power: ~40W**

## Boot Sequence

1. Power on autoclip — starts video.service, waits for stream connection
2. Power on autowaaave — starts stream_receive, autowaaave (OF app), video_bridge
3. Power on automidi — displays boot sequence, waits for "START" from autowaaave
4. video_bridge detects OF app running, sends "START" to Teensy
5. automidi syncs all parameters via MIDI CC sweep
6. System operational

## Enclosure Considerations

- automidi: Custom panel with encoder/button cutouts, OLED windows
- autoclip: Ventilated case, USB ports accessible for capture cards
- autowaaave: HiFiBerry-compatible case with audio jack access

## Next Steps

1. **automidi/BUILDME.md** — Detailed Teensy wiring, shift register pinouts, display connections
2. **autoclip/BUILDME.md** — Capture card compatibility, Pi 5 setup
3. **autowaaave/BUILDME.md** — HiFiBerry setup, openFrameworks installation
