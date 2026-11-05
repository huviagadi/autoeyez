# automidi

The Teensy 4.1 firmware in `auto_midi_v0911/` is AutoEyez's physical control
surface. It reads nine encoders and twelve buttons, drives nine OLEDs and
eight LEDs, analyzes line input through a PJRC Audio Shield, and stores
video/audio patches on the Teensy's SD card.

USB MIDI sends renderer parameters and low/mid/high FFT bands. The bands use
CC 110–112 and a CC 113 frame marker; extended values use CC 114–119.
USB serial sends playback, mixer, and patch commands to the bridge on
autowaaave and receives state for the displays.

The current firmware code has **32 video and 32 audio patch slots**. Its
64-entry clip-name buffer is a separate limit. Three UI pages cover renderer
controls, clip playback, mixer/source selection, patches, and processing.
The firmware source defines the actual menu and CC mappings.

Build for Teensy 4.1 with USB MIDI and serial enabled. Dependencies are
Teensyduino, Teensy Audio, Encoder, Adafruit GFX, and Adafruit SSD1306.
The source uses `Adafruit_SSD1306` objects for SH1106 OLED modules.
Check the physical controller before using older wiring instructions.

See [the system architecture](../ARCHITECTURE.md) for connections and the
source-versus-deployment boundary.
