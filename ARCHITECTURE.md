architecture_notes.md

Network / SSH Access
	•	autowaaave Pi (Pi 3B+): autowaaave.local — user: admin, password: admin
	•	autoclip Pi (Pi 5): autoclip.local / 10.0.0.2 — user: admin, password: admin
	•	SSH key auth configured from Mac (no password needed for SSH)
	•	NOPASSWD sudo enabled for admin user on autowaaave Pi

Video Streaming (autoclip → autowaaave)
	•	Low-latency mode: MJPEG over UDP (was H.264/MPEG-TS/TCP)
	•	Sender (autoclip): mixer.py → ffmpeg mjpeg → udp://10.0.0.1:1236
	•	Receiver (autowaaave): stream_receive.service → ffmpeg → /dev/video10 (v4l2loopback)
	•	Backup of original receiver: /etc/systemd/system/stream_receive.service.bak

Deployment (autowaaave)
	•	Script: ./deploy_autowaaave.sh — copies ofApp.cpp, builds, restarts service
	•	Manual: scp file to /tmp, then ssh to copy/build/restart
	•	DO NOT use: sshpass (not installed), expect with sudo (password prompts break)
	•	Build command: cd /home/pi/openFrameworks/apps/myApps/AUTO_WAAAVE_4_5 && sudo make -j4
	•	Restart: sudo systemctl restart autowaaave

Hardware Platform
	•	Teensy 4.1 running at 600MHz with 1MB RAM and 8MB flash
	•	PJRC Audio Shield Rev D stacked on Teensy header pins
	•	VUSB trace intentionally cut — Teensy powered via external 5V on VIN pin, USB used only for MIDI data
	•	AMS1117-3.3 voltage regulator powered from VUSB (5V), supplies 3.3V rail to all OLEDs and shift registers independently of Teensy onboard regulator (which failed once from overload)
	•	Bootloader access requires: power on via 5V rail first, then connect USB data cable while holding program button for ~20 seconds until dim flickering LED appears
Display System
	•	9x SH1106 OLED displays driven with Adafruit SSD1306 library (controllers are SH1106 not SSD1306 despite vendor listing)
	•	All displays share SPI bus: SCK pin 13, MOSI pin 11, DC pin 15, RES pin 29
	•	Individual CS pins: display 0-6 on pins 0-6, display 7 on pin 9, display 8 on pin 14
	•	33Ω series resistors on SCK, MOSI and DC lines for signal integrity — critical, removing them causes display jitter
	•	104 ceramic decoupling capacitors on each display VCC to GND
	•	RES line previously caused issues when shared — now managed manually with pinMode/digitalWrite before begin()
	•	Adafruit SSD1306 begin() call: displays[i].begin(SSD1306_SWITCHCAPVCC, 0, false, true) — third param false prevents library touching RES, fourth param true initializes SPI
	•	Garbage pixels appear on right edge of displays (SH1106 has 132 column RAM, display shows 128) — partially mitigated with signal line capacitors
	•	Display objects declared as global array of 9 Adafruit_SSD1306 instances
	•	Each display() call transfers full 1KB frame buffer over SPI taking ~1300µs
	•	Critical: maximum one display update per loop iteration to prevent audio interrupt starvation
	•	All display calls wrapped in AudioNoInterrupts/AudioInterrupts
	•	Dirty flag system: arrays labelUpdated[9] and graphUpdated[8] track which screens need redrawing
	•	On page change all dirty flags set to false triggering full redraw over subsequent loop iterations
	•	Normal loop time: ~345µs, with one display update: ~1600µs, must stay under 2900µs
Audio System
	•	SGTL5000 codec on audio shield handles line in via solderable pads on shield PCB
	•	AudioAnalyzeFFT256 provides 128 frequency bins at 44100Hz sample rate = 172Hz per bin
	•	FFT display uses first 32-64 bins covering ~5.5-11kHz musical range
	•	Fast attack/slow decay smoothing applied to FFT bins for visual feel
	•	AudioMemory set to 16 — higher values needed to survive occasional 2800µs loop spikes
	•	Spectrum drawn on display 8 (screen 9) only on page 0
	•	AudioControlSGTL5000 global object declaration order matters — must come after display array declaration or Teensy hangs on boot
	•	Known instability: page changes can crash system due to audio interrupt starvation during burst of display updates — staggered update system partially mitigates this
Button System
	•	Two 74HC165 input shift registers daisy chained
	•	Chip 1 handles: standalone buttons 1-3 (bits 0-2), encoder SW 1-5 (bits 3-7)
	•	Chip 2 handles: encoder SW 6-9 (bits 8-11), unused inputs tied to 3.3V (bits 12-15)
	•	10kΩ pullup resistors required on all 12 active inputs — 74HC165 has no internal pullups, floating inputs cause erratic behavior
	•	Button reading uses bitwise edge detection: pressed, released, held, notPressed
	•	Debounce implemented in software with 5ms stability window
	•	Data flows: Chip1 QH (pin9) → Chip2 SER (pin10) → Chip2 QH (pin9) → Teensy pin 26
	•	Button bit order confirmed empirically — standalone buttons bits 0-2, encoders bits 3-11
	•	74HC165 chips are fragile — one failed during development from likely static discharge or voltage spike. Series resistors on inputs recommended for future protection
	•	Mask 0x0FFF applied to button reads to ignore unused upper bits
LED System
	•	74HC595 output shift register on pins 22 (data), 24 (clk), 25 (latch)
	•	LED physical to bit mapping (confirmed empirically):
	◦	Bit 0 = Button LED 1
	◦	Bit 1 = Button LED 2
	◦	Bit 2 = Button LED 3
	◦	Bit 3 = Standalone LED 1
	◦	Bit 4 = Standalone LED 2
	◦	Bit 5 = Standalone LED 3
	◦	Bit 6 = Standalone LED 4
	◦	Bit 7 = Standalone LED 5
	•	Standalone button LEDs are 5-12V rated with internal current limiting — driven directly from 3.3V shift register output, light dimly but acceptably
	•	Power-on reset: shift register outputs float before Teensy initializes causing LEDs to flicker — mitigated by immediately shifting 0x00 as first setup() action
	•	LED states managed via bool LED_ON[8] array, converted to byte and shifted out each loop
Encoder System
	•	9x rotary encoders with push button on pins 30-47 (CLK/DT pairs)
	•	Encoder library by Paul Stoffregen used for reliable pulse counting
	•	Encoders accessed via pointer array: Encoder* encoders[9] = {&enc0, &enc1, ...}
	•	Direction inverted from default — increment uses -1 : 1 not 1 : -1
	•	encoder_speed multiplier applied to value changes for faster response
	•	Values constrained to 0-127 for MIDI CC compatibility
	•	function_value[16] stores current value for all 16 parameters across both pages
	•	function_reset[16] stores default reset values, applied via memcpy on reset button
MIDI System
	•	USB MIDI via Teensy native USB — set Tools → USB Type → MIDI or Serial+MIDI
	•	Teensy appears as standard USB MIDI device, no drivers needed on Mac/Linux
	•	sendControlChange(ccNumber, value, channel) for encoder values
	•	sendNoteOn/sendNoteOff for function button state changes
	•	usbMIDI.send_now() called after every message
	•	cc_encoder[16] array maps encoder index to CC number
	•	cc_control[8] array maps control button index to CC number
	•	cc_reactive[3] array for reactive/standalone button CCs
	•	MIDI channel 1 used throughout
	•	Known bug: patch loading updates UI values but does not send MIDI CC messages to auto_waaave — values display correctly on screens but auto_waaave parameters are not updated
Page System
	•	bool page toggles between 0 and 1 on BTN_MENU release
	•	Page 0: label[] array, parameters 0-7, encoders map to function_value[0-7]
	•	Page 1: label_B[] array, parameters 8-15, encoders map to function_value[8-15]
	•	Parameter index formula: temp_function = (8 * page) + encoder_index
	•	Control buttons: 4 per page, index formula temp_control = (4 * page) + i
	•	Function buttons: 4 per page cycling through 4 states (0-3), index formula temp_function = (4 * page) + i
	•	LED_ON[LED_5] indicates current page state
	•	Page change sets all dirty flags for full screen redraw
Known Issues and Gotchas
	•	Audio crashes on rapid page changes — audio interrupt starvation from burst display updates
	•	SH1106 garbage pixels on right edge of all displays
	•	Patch load doesn't apply values to auto_waaave via MIDI
	•	Teensy bootloader access is non-trivial due to cut VUSB trace — requires specific power-on sequence
	•	Two copies of Encoder library found during development — keep only Teensy packages version
	•	AudioControlSGTL5000 global declaration order sensitive — crashes if declared before display array
	•	74HC165 inputs must have external pullups — no internal pullups on this chip
	•	SPI signal resistors are critical — do not remove
	•	Power budget: OLEDs + shift registers + LEDs draw ~265mA, AMS1117 handles this comfortably at 1A rating
Development Environment
	•	Arduino IDE 2.3.8
	•	Teensyduino 1.60
	•	Libraries: Adafruit SSD1306, Adafruit GFX, Encoder (Teensy packages version), Teensy Audio
	•	Upload via Teensy Loader for reliable bootloader access
	•	Serial monitor at 115200 baud for debugging
