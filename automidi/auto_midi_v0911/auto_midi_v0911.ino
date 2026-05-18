/*
 * =============================================================================
 * automidi — Teensy 4.1 MIDI Controller Firmware v0.9.11
 * Part of the autoeyez video synthesis system
 * =============================================================================
 *
 * This firmware drives the physical control surface for the autoeyez video
 * synthesizer. It reads encoders and buttons, displays parameter values on
 * OLEDs, sends MIDI CC messages to autowaaave for shader control, and sends
 * serial commands to autoclip for video playback control.
 *
 * HARDWARE:
 *   - Teensy 4.1 @ 600MHz with Audio Shield Rev D (for line-in FFT analysis)
 *   - 9× rotary encoders with pushbuttons (encoder pins 30-47, buttons via shift registers)
 *   - 9× SH1106 128×64 OLED displays (SPI bus, CS pins 0-6, 9, 14)
 *   - 2× 74HC165 shift registers for 12 button inputs (active low)
 *   - 1× 74HC595 shift register for 8 LED outputs
 *   - SD card (Teensy built-in) for patch storage
 *
 * COMMUNICATION:
 *   USB MIDI Channel 1:
 *     - CC 16-127: Shader parameters to autowaaave openFrameworks app
 *     - CC 91: Patch load latch (127=start, 0=end)
 *     - CC 92-93: Audio patch save/load
 *
 *   USB Serial 115200:
 *     - Commands TO autoclip (via video_bridge on autowaaave):
 *       NEXT, PREV, PLAY, PAUSE, LOOP_ON, LOOP_OFF, PLAY:n
 *       CH_A:n, CH_B:n, MIX:n, LUMA:n, LUMA_SRC:n, LUMA_HIGH:n, LUMA_HIGH_EN:n
 *     - Messages FROM autoclip:
 *       FILE:name, POS:idx:count, PAUSE:0/1, LOOP:0/1, LIST:name1,name2,...
 *       PROGRESS:pos:dur, START (system ready signal)
 *
 * PAGES (three control modes):
 *   Page 0 - AUDIO/FX:
 *     Encoders 0-7: Luma, Blend, Hue, Saturation, X Offset, Y Offset, Scale, Angle
 *     Encoder 8: Line-in volume with FFT spectrum display
 *     Buttons 1-4: Toggle controls (invert luma/hue/sat, toroidal wrap)
 *     Buttons 5-8: Function multipliers (0×, 2×, 5×, 10× modulation)
 *     Ring buttons: Audio-reactive band select (Low/Mid/High)
 *
 *   Page 1 - VIDEO/CLIP:
 *     Encoders 0-7: Value, Blur, Bloom, Sharpen, Hue Modulo/LFO/Offset, Delay Time
 *     Encoder 8: Clip list navigation and playback control
 *     Ring buttons: Loop toggle, Play/Pause, Skip to next clip
 *
 *   Page 2 - MIXER/LUMA KEY:
 *     Encoder 0: Luma key low threshold
 *     Encoder 1: Luma key high threshold
 *     Encoder 2: Fade to black
 *     Encoder 4: Luma key source select
 *     Encoder 5: Video patch browser
 *     Encoder 6: Audio patch browser
 *     Encoder 8: A/B crossfader or channel source select
 *     Ring buttons: Select Ch A / Mixer view / Ch B
 *
 * PATCH SYSTEM:
 *   - 32 video patches stored on SD card (PATCH01.TXT through PATCH32.TXT)
 *   - 32 audio patches stored in autowaaave (triggered via MIDI CC 92/93)
 *   - Hold encoder button to save, tap to load
 *   - Patches store all encoder values, toggle states, mixer settings
 *
 * DISPLAY UPDATE STRATEGY:
 *   To avoid starving the audio FFT analysis, only one display is updated per
 *   main loop iteration. The graphUpdated[], labelUpdated[], etc. flags track
 *   which displays need refresh, and the draw queue processes them one at a time.
 *
 * =============================================================================
 */

// ============================================================
// INCLUDES
// ============================================================
#include <Encoder.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Audio.h>
#include <Wire.h>
#include <SD.h>

// ============================================================
// PIN DEFINITIONS & CONSTANTS
// ============================================================
// Hardware pin assignments for all peripherals connected to Teensy 4.1.
// See BUILDME.md for wiring diagrams and shift register connections.

// SD Card (using Teensy built-in SD slot)
#define SD_CS 10              // Chip select for SD card (Audio Shield shares SPI)
#define PATCH_HOLD_MS 1500    // Hold time for patch save (vs tap for load)
#define PATCH_FLASH_MS 1000   // Duration of "SAVED!" / "LOADED!" feedback

// Display dimensions
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

// 74HC595 LED shift register pins (active high output)
// Single 595 controls 8 LEDs on the control surface
#define LED_DATA 22           // Serial data input (SER/DS)
#define LED_CLOCK 24          // Shift clock (SRCLK/SH_CP)
#define LED_LATCH 25          // Storage latch (RCLK/ST_CP)

// LED bit positions in the 595 output byte
// Physical layout: LEDs 1-4 are function indicators, 5-8 are control toggles
#define LED_1 3               // Function LED 1 (modulation active)
#define LED_2 4               // Function LED 2
#define LED_3 5               // Function LED 3
#define LED_4 6               // Function LED 4
#define LED_5 7               // Control LED 5
#define LED_6 0               // Control LED 6 / Page 0 indicator
#define LED_7 1               // Control LED 7 / Page 1 indicator
#define LED_8 2               // Control LED 8 / Page 2 indicator

// 74HC165 button shift register pins (active low input)
// Two 165s daisy-chained for 16 bits (only 12 buttons used)
#define BUTTON_DATA 26        // Serial data output (QH)
#define BUTTON_CLOCK 16       // Shift clock (CLK)
#define BUTTON_LOAD 17        // Parallel load (SH/LD, active low)

// Button indices in the 16-bit shift register data
// Buttons are active-low, so pressed = 0, released = 1
#define BTN_LOW  0            // Ring button: Low band / Page 0
#define BTN_MID  1            // Ring button: Mid band / Page 1
#define BTN_HI   2            // Ring button: High band / Page 2
#define BTN_1    3            // Encoder 0 push button
#define BTN_2    4            // Encoder 1 push button
#define BTN_3    5            // Encoder 2 push button
#define BTN_4    6            // Encoder 3 push button
#define BTN_5    7            // Encoder 4 push button
#define BTN_6    8            // Encoder 5 push button
#define BTN_7    9            // Encoder 6 push button
#define BTN_8    10           // Encoder 7 push button
#define BTN_MENU 11           // Encoder 8 push button (page/menu)

#define DEBOUNCE_MS 5
#define GRAPH_Y 26
#define GRAPH_H 12
#define PAGE_HOLD_MS 300      // Hold time to enter page-select mode

// ============================================================
// VIDEO COMMAND QUEUE
// ============================================================
// Commands to autoclip are queued to avoid overwhelming the serial link.
// The queue is flushed at a rate slower than autoclip's poll interval.
#define VIDEO_CMD_QUEUE_SIZE 12
char videoCmdQueue[VIDEO_CMD_QUEUE_SIZE][24] ;  // Circular buffer of commands
int  videoCmdHead = 0 ;                          // Read position
int  videoCmdTail = 0 ;                          // Write position

// Patch loading state machine
// When loading a patch, MIDI values are sent one at a time to avoid
// overwhelming the autowaaave app. patchLoadStep tracks progress.
bool patchLoadPending = false ;   // True while patch load is in progress
int  patchLoadStep    = 0 ;       // Current step in the load sequence
unsigned long patchLoadTimer = 0 ;// Throttle timer for staggered sends

// ============================================================
// ENCODER SETUP
// ============================================================
// 9 quadrature encoders connected to consecutive pin pairs.
// Each encoder uses 2 pins for A/B quadrature signals.
// The Encoder library handles interrupt-based position tracking.

Encoder enc0(30, 31) ;    // Top row, left
Encoder enc1(32, 33) ;    // Top row, center-left
Encoder enc2(34, 35) ;    // Top row, center
Encoder enc3(36, 37) ;    // Top row, center-right
Encoder enc4(38, 39) ;    // Bottom row, left
Encoder enc5(40, 41) ;    // Bottom row, center-left
Encoder enc6(42, 43) ;    // Bottom row, center
Encoder enc7(44, 45) ;    // Bottom row, center-right
Encoder enc8(46, 47) ;    // Master encoder (bottom right)

Encoder* encoders[9] = { &enc0, &enc1, &enc2, &enc3, &enc4, &enc5, &enc6, &enc7, &enc8 } ;

// Encoder sensitivity: pulses per value step (higher = slower)
int encoder_speed = 2 ;

// Last encoder position for delta calculation
// 18 slots: encoders 0-8 on page 0 (indices 0-8), encoders 0-8 on page 1 (indices 9-17)
long lastEncPos[18] = {0} ;

// Parameter values for all 18 encoder functions (9 per page × 2 pages)
// These are sent as MIDI CC values (0-127 range)
// Index mapping: Page 0 = indices 0-8, Page 1 = indices 9-17
int  function_value[18]     { 0, 64, 63, 63, 63, 63, 63, 63, 100,   63, 63, 0, 0, 0, 63, 63, 0, 63 } ;

// Reset/default values for each parameter (used by global reset function)
int  function_reset[18]     { 0, 64, 63, 63, 63, 63, 63, 63, 100,   63, 63, 0, 0, 0, 63, 63, 0, 63 } ;

// Encoder direction: 1 = CW increases, -1 = CW decreases (for inverted UX)
int  function_direction[18] { 1, 1, 1, 1, -1, -1, 1, -1, 1,        1, 1, 1, 1, 1, 1, 1, 1, 1 } ;

// Graph display type: 0 = bar from left, 1 = center-out bipolar
bool function_graphType[18] { 0, 1, 1, 1, 1, 1, 1, 1, 1,           1, 1, 0, 0, 0, 1, 1, 0, 1 } ;

// MIDI CC numbers for each encoder parameter
// These map to parameters in the autowaaave openFrameworks shader app
int  cc_encoder[18]         { 16, 17, 18, 19, 120, 121, 122, 123, 7,   20, 21, 22, 23, 124, 125, 126, 127, 0 } ;

unsigned long lastFrameTime = 0 ;
float currentFPS = 0 ;
float smoothedFPS = 0 ;
#define FPS_SMOOTH 0.95f

bool booting = true ;
bool booted = false ;
int boot_time_seconds = 80 ;
bool autowaaaveReady = false ;
bool scroll_LEDs = true ;
bool done = false ;

uint16_t stableButtons = 0x0FFF ;
uint16_t lastRawButton = 0x0FFF ;
unsigned long lastChangeTime = 0 ;
uint16_t lastButtons = 0x0FFF ;
uint16_t held = 0 ;
uint16_t buttons = 0x0FFF ;
int  button[12] = {0} ;
bool LED_ON[8] = {false} ;
bool savedLED[8] = {false} ;
int  page = 0 ;
int  function[8] { 0 } ;
bool function_type[8] = { 0, 0, 0, 0, 0, 0, 0, 1 } ;
int  function_flash[4] { 0 } ;
int  react_band = -1 ;
bool control[8] = { false, false, false, false, false, false, false, false } ;
bool control_type[8] = { 0, 0, 0, 0, 0, 0, 0, 1 } ;

int cc_reactive[3] = { 43, 44, 42 } ;
int cc_control[8] = { 60, 46, 62, 61, 41, 45, 71, 59 } ;

bool labelUpdated[9]     = { false, false, false, false, false, false, false, false, false } ;
bool graphUpdated[9]     = { false, false, false, false, false, false, false, false, false } ;
bool controlUpdated[4]   = { false, false, false, false } ;
bool functionUpdated[4]  = { false, false, false, false } ;

char clipNames[32][20] ;
int  clipCount      = 0 ;
int  menuScroll     = 0 ;
int  menuSelection  = 0 ;
bool videoDisplayUpdated = false ;
bool isPlaying      = true ;
bool isLooping      = false ;
int  clipPos        = 0 ;
int  clipTotal      = 0 ;
char nowPlaying[20] = "..." ;
unsigned long skipLedTime = 0 ;
#define SKIP_LED_MS 200
int progressPos   = 0 ;
int progressTotal = 0 ;
unsigned long menuPressTime = 0 ;

char serialBuf[256] ;
int  serialBufPos = 0 ;

// ============================================================
// MIXER STATE
// ============================================================
// The mixer on autoclip has two channels (A and B) that can be sourced
// from clips, composite inputs, or oscilloscope. These settings control
// the video mixer via serial commands sent through video_bridge.

#define NUM_SOURCES 5
const char* sourceNames[NUM_SOURCES] = {
  "CLIPS", "COMPOSITE 1", "COMPOSITE 2", "COMPOSITE 3", "OSCILLOSCOPE"
} ;

// Luma key source includes "NONE" as first option
#define NUM_LUMA_SOURCES 6
const char* lumaSourceNames[NUM_LUMA_SOURCES] = {
  "NONE", "CLIPS", "COMPOSITE 1", "COMPOSITE 2", "COMPOSITE 3", "OSCILLOSCOPE"
} ;

// Mixer page sub-modes (selected by ring buttons on page 2)
// 0 = Channel A source select, 1 = Mixer/crossfader view, 2 = Channel B source select
int  mixerSubMode      = 1 ;   // Default to mixer view

// Channel source assignments (which source feeds each channel)
int  chASource         = 0 ;   // Channel A current source (sent to autoclip)
int  chBSource         = 1 ;   // Channel B current source

// Channel source selection (cursor position in source list, not yet confirmed)
int  chASelection      = 0 ;   // Channel A selection cursor
int  chBSelection      = 0 ;   // Channel B selection cursor
int  chAScroll         = 0 ;   // Channel A list scroll offset
int  chBScroll         = 0 ;   // Channel B list scroll offset

// Crossfade and luma key parameters
int  mixValue          = 63 ;   // A/B crossfade: 0=full A, 127=full B
int  lumaKeyValue      = 0 ;    // Luma key low threshold (0-127)
int  lumaHighValue     = 127 ;  // Luma key high threshold (0-127)
bool lumaHighEnabled   = false ;// Enable high threshold cutoff
bool lumaLowEnabled    = false ;// Enable low threshold (luma key active)
static long enc5Accum         = 0 ;
int      patchCursor          = 0 ;
int      patchScroll          = 0 ;
bool     patchFilled[32]      = { false } ;
char     patchNames[32][10] ;
unsigned long btn6PressTime   = 0 ;
bool     btn6HeldLong         = false ;
bool     patchFlashActive     = false ;
unsigned long patchFlashTimer = 0 ;
bool     patchFlashSaved      = false ;
static long enc6Accum           = 0 ;
int      audioPatchCursor       = 0 ;
int      audioPatchScroll       = 0 ;
bool     audioPatchFilled[32]   = { false } ;
char     audioPatchNames[32][10] ;
unsigned long btn7PressTime     = 0 ;
bool     btn7HeldLong           = false ;
bool     audioPatchFlashActive  = false ;
unsigned long audioPatchFlashTimer = 0 ;
bool     audioPatchFlashSaved   = false ;
int srcHue    = 0 ;
int srcSat    = 0 ;
int srcVal    = 0 ;
int srcShader = 0 ;
bool page2ClearUpdated[2] = { false, false } ;
static long enc1Accum = 0 ;
int  lumaSourceSel     = 0 ;
int  lumaSourceScroll  = 0 ;
int  lumaSourceCursor  = 0 ;
bool suppressDisplay8Draw = false ;  // true while previewing page on display 8

float mixValueSmoothed = 0.0f ;  // smoothed display value
#define MIX_SMOOTH 0.003f         // 0.0=instant, lower=slower

#define P2_LUMA      0
#define P2_LUMA_HI   1
#define P2_LUMA_SRC  2
#define P2_PATCH     3
#define P2_AUDIO     4
#define P2_MIXER     5
#define P2_FADE      6
#define P2_COUNT     7
bool page2Updated[P2_COUNT] = { false } ;
int  fadeToBlackValue    = 0 ;
static long enc2Accum    = 0 ;

// Page switching
int  pendingPage     = -1 ;
bool enc8HeldLong    = false ;
bool pageSwitched    = false ;
bool ledOverride     = false ;  // true when showing page select LEDs

// Encoder accumulators
static long enc0Accum = 0 ;
static long enc4Accum = 0 ;
static long enc8Accum = 0 ;

// Flash timer
unsigned long pageFlashTimer = 0 ;
bool pageFlashState = true ;
#define PAGE_FLASH_MS 150

int csPins[] = {0, 1, 2, 3, 4, 5, 6, 9, 14} ;

const char* label[] = {
  "LUMA", "BLEND", "HUE", "SATURATION",
  "X OFFSET", "Y OFFSET", "SCALE", "ANGLE", "AUDIO"
} ;
const char* label_B[] = {
  "VALUE", "BLUR", "BLOOM", "SHARPEN",
  "HUE MODULO", "HUE LFO", "HUE OFFSET", "DELAY TIME", "VIDEO"
} ;
const char* boot_label[] = {
  "A", "U", "T", "O", "E E E", "Y Y Y", "E E E", "Z Z Z", "<3\n    &"
} ;
const char* control_label[] = {
  "INVERT LUMA", "TOROIDAL", "INVERT HUE", "INVERT SAT",
  "H MIRROR", "V MIRROR", "WET/DRY", "AUDIO & VIDEO"
} ;
const char* function_label[]  = { "0x", "2x", "5x", "10x" } ;

Adafruit_SSD1306 displays[9] {
  Adafruit_SSD1306(SCREEN_WIDTH, SCREEN_HEIGHT, &SPI, 15, -1, 0),
  Adafruit_SSD1306(SCREEN_WIDTH, SCREEN_HEIGHT, &SPI, 15, -1, 1),
  Adafruit_SSD1306(SCREEN_WIDTH, SCREEN_HEIGHT, &SPI, 15, -1, 2),
  Adafruit_SSD1306(SCREEN_WIDTH, SCREEN_HEIGHT, &SPI, 15, -1, 3),
  Adafruit_SSD1306(SCREEN_WIDTH, SCREEN_HEIGHT, &SPI, 15, -1, 4),
  Adafruit_SSD1306(SCREEN_WIDTH, SCREEN_HEIGHT, &SPI, 15, -1, 5),
  Adafruit_SSD1306(SCREEN_WIDTH, SCREEN_HEIGHT, &SPI, 15, -1, 6),
  Adafruit_SSD1306(SCREEN_WIDTH, SCREEN_HEIGHT, &SPI, 15, -1, 9),
  Adafruit_SSD1306(SCREEN_WIDTH, SCREEN_HEIGHT, &SPI, 15, -1, 14)
} ;

AudioControlSGTL5000 audioShield ;
AudioInputI2S audioIn ;
AudioMixer4 mixer ;
AudioAnalyzeFFT256 fft ;
AudioConnection patchCord1(audioIn, 0, mixer, 0) ;
AudioConnection patchCord2(audioIn, 1, mixer, 1) ;
AudioConnection patchCord3(mixer, 0, fft, 0) ;

float smoothedLevels[128] = { 0 } ;
float peakLevels[32] = { 0 } ;
unsigned long peakHoldStart[32] = { 0 } ;
#define PEAK_HOLD_MS 1000
#define PEAK_DECAY   0.0012f
#define ATTACK  0.8
#define DECAY 0.09

unsigned long lastMidiWatchdog = 0 ;
#define MIDI_WATCHDOG_MS 2000
bool midiInitialized = false ;

// ============================================================
// SERIAL COMMUNICATION
// ============================================================
// USB Serial handles bidirectional communication:
//   TX: Commands to autoclip (via video_bridge on autowaaave)
//   RX: State updates from autoclip (clip names, positions, etc.)

/**
 * Send a command immediately to autoclip via serial.
 * Use for real-time controls that need immediate response.
 */
void sendVideoCommand(const char* cmd) { Serial.println(cmd) ; }

/**
 * Queue a command for rate-limited sending to autoclip.
 * Use for batch operations (e.g., patch load) to avoid overwhelming the link.
 * Circular buffer drops commands if full (shouldn't happen in normal use).
 */
void queueVideoCommand(const char* cmd) {
  int next = (videoCmdTail + 1) % VIDEO_CMD_QUEUE_SIZE ;
  if (next != videoCmdHead) {
    strncpy(videoCmdQueue[videoCmdTail], cmd, 23) ;
    videoCmdQueue[videoCmdTail][23] = '\0' ;
    videoCmdTail = next ;
  }
}

/**
 * Send one queued command per call, rate-limited to 15ms intervals.
 * Called from main loop. Pauses during patch loading to allow MIDI traffic.
 */
void flushVideoQueue() {
  static unsigned long lastFlush = 0 ;
  if (patchLoadPending) return ;              // Pause queue during patch load
  if (videoCmdHead == videoCmdTail) return ;  // Queue empty
  if (millis() - lastFlush < 15) return ;     // Rate limit (mixer polls at 10ms)
  lastFlush = millis() ;
  Serial.println(videoCmdQueue[videoCmdHead]) ;
  videoCmdHead = (videoCmdHead + 1) % VIDEO_CMD_QUEUE_SIZE ;
}

/**
 * Parse and handle incoming serial messages from autoclip.
 * Messages are forwarded by video_bridge.py on autowaaave.
 *
 * Message formats:
 *   START           - System ready signal (ends boot wait)
 *   FILE:name       - Current clip name for display
 *   PAUSE:0/1       - Playback state (0=playing, 1=paused)
 *   LOOP:0/1        - Loop mode state
 *   POS:idx:count   - Clip position in playlist
 *   LIST:a,b,c,...  - Full clip list (comma-separated names)
 *   PROGRESS:p:d    - Playback position (seconds) and duration
 */
void handleSerialMessage(const char* msg) {
  // System ready signal from video_bridge (autowaaave is running)
  if (strncmp(msg, "START", 5) == 0) {
    autowaaaveReady = true ;
    Serial.println("GOT_START") ;
    return ;
  }

  // Current clip name for OLED display
  if (strncmp(msg, "FILE:", 5) == 0) {
    strncpy(nowPlaying, msg + 5, 19) ; nowPlaying[19] = '\0' ;
    videoDisplayUpdated = false ;
  }
  // Playback state (inverse: PAUSE:0 means playing)
  else if (strncmp(msg, "PAUSE:", 6) == 0) {
    isPlaying = (atoi(msg + 6) == 0) ;
    videoDisplayUpdated = false ;
    if (page == 1) { LED_ON[LED_7] = isPlaying ; }
  }
  // Loop mode state
  else if (strncmp(msg, "LOOP:", 5) == 0) {
    isLooping = (atoi(msg + 5) == 1) ;
    videoDisplayUpdated = false ;
    if (page == 1) { LED_ON[LED_6] = isLooping ; }
  }
  // Position in clip list (index:total)
  else if (strncmp(msg, "POS:", 4) == 0) {
    char* colon = strchr(msg + 4, ':') ;
    if (colon) { clipPos = atoi(msg + 4) ; clipTotal = atoi(colon + 1) ; }
    videoDisplayUpdated = false ;
  }
  // Full clip list (sent on startup and periodically)
  else if (strncmp(msg, "LIST:", 5) == 0) {
    clipCount = 0 ;
    char buf[256] ; strncpy(buf, msg + 5, 255) ; buf[255] = '\0' ;
    char* token = strtok(buf, ",") ;
    while (token != NULL && clipCount < 32) {
      strncpy(clipNames[clipCount], token, 19) ; clipNames[clipCount][19] = '\0' ;
      clipCount++ ; token = strtok(NULL, ",") ;
    }
    videoDisplayUpdated = false ;
  }
  // Playback progress (position:duration in seconds)
  else if (strncmp(msg, "PROGRESS:", 9) == 0) {
    char* colon = strchr(msg + 9, ':') ;
    if (colon) { progressPos = atoi(msg + 9) ; progressTotal = atoi(colon + 1) ; }
    videoDisplayUpdated = false ;
  }
}

void checkSerial() {
  while (Serial.available()) {
    char c = Serial.read() ;
    if (c == '\n') {
      serialBuf[serialBufPos] = '\0' ;
      if (serialBufPos > 0) {
        if (serialBuf[0] == 'R' && serialBufPos == 1) { _reboot_Teensyduino_() ; }
        else { handleSerialMessage(serialBuf) ; }
      }
      serialBufPos = 0 ;
    } else if (serialBufPos < 255) { serialBuf[serialBufPos++] = c ; }
  }
}

// ============================================================
// DISPLAY FUNCTIONS — EXISTING
// ============================================================

uint16_t readButtons() {
  digitalWrite(BUTTON_LOAD, LOW) ; delayMicroseconds(10) ;
  digitalWrite(BUTTON_LOAD, HIGH) ; delayMicroseconds(10) ;
  uint16_t result = 0 ;
  for(int i = 15 ; i > -1 ; i--) {
    result |= (digitalRead(BUTTON_DATA) << i) ;
    digitalWrite(BUTTON_CLOCK, HIGH) ; delayMicroseconds(10) ;
    digitalWrite(BUTTON_CLOCK, LOW) ;  delayMicroseconds(10) ;
  }
  return result ;
}

void setLEDs(byte ledByte) {
  digitalWrite(LED_LATCH, LOW) ;
  shiftOut(LED_DATA, LED_CLOCK, MSBFIRST, ledByte) ;
  digitalWrite(LED_LATCH, HIGH) ;
}

void clearScreen(int display_id) {
  displays[display_id].fillRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_BLACK) ;
  displays[display_id].display() ;
}

void updateLabel(int display_id, const char* text, int textSize) {
  displays[display_id].fillRect(0, 0, SCREEN_WIDTH, 20, SSD1306_BLACK) ;
  displays[display_id].setTextColor(SSD1306_WHITE) ;
  displays[display_id].setTextSize(textSize) ;
  int16_t x1, y1 ; uint16_t w, h ;
  displays[display_id].getTextBounds(text, 0, 0, &x1, &y1, &w, &h) ;
  displays[display_id].setCursor((SCREEN_WIDTH - w) / 2, 0) ;
  displays[display_id].println(text) ;
  displays[display_id].display() ;
}

void updateControlLabel(int display_id, const char* text, int textSize, int type, int value) {
  displays[display_id].fillRect(0, 47, SCREEN_WIDTH, 17, SSD1306_BLACK) ;
  displays[display_id].setTextColor(SSD1306_WHITE) ;
  displays[display_id].setTextSize(textSize) ;
  displays[display_id].setCursor(6, 48) ; displays[display_id].println(text) ;
  if (type == 0) {
    if (!value) { displays[display_id].fillRect(83, 47, 19, 9, SSD1306_WHITE) ; displays[display_id].setTextColor(SSD1306_BLACK) ; }
    displays[display_id].setCursor(84, 48) ; displays[display_id].println("OFF") ;
    displays[display_id].setTextColor(SSD1306_WHITE) ;
    if (value)  { displays[display_id].fillRect(107, 47, 14, 9, SSD1306_WHITE) ; displays[display_id].setTextColor(SSD1306_BLACK) ; }
    displays[display_id].setCursor(108, 48) ; displays[display_id].println("ON") ;
    displays[display_id].setTextColor(SSD1306_WHITE) ;
  } else if (type == 1) {
    if (value) { displays[display_id].fillRect(87, 47, 32, 9, SSD1306_WHITE) ; displays[display_id].setTextColor(SSD1306_BLACK) ; }
    displays[display_id].setCursor(89, 48) ; displays[display_id].println("RESET") ;
    displays[display_id].setTextColor(SSD1306_WHITE) ;
  }
  displays[display_id].display() ;
}

void updateFunctionLabel(int display_id, const char* text, int textSize, int type, int function_id) {
  displays[display_id].fillRect(0, 47, SCREEN_WIDTH, 17, SSD1306_BLACK) ;
  displays[display_id].setTextColor(SSD1306_WHITE) ;
  displays[display_id].setTextSize(textSize) ;
  for(int i = 0 ; i < 4 ; i++) {
    if (function[function_id] == i) { displays[display_id].fillRect(13 + (28 * i), 47, 15 + (6 * (i == 3)), 9, SSD1306_WHITE) ; displays[display_id].setTextColor(SSD1306_BLACK) ; }
    displays[display_id].setCursor(14 + (28 * i), 48) ; displays[display_id].println(function_label[i]) ;
    if (function[function_id] == i) { displays[display_id].setTextColor(SSD1306_WHITE) ; }
  }
  displays[display_id].display() ;
}

void updateGraph(int display_id, int value, int function_id) {
  displays[display_id].fillRect(0, 19, SCREEN_WIDTH, 22, SSD1306_BLACK) ;
  if (function_direction[function_id] == -1) { value = 127 - value ; }
  int left  = constrain(map(value, 1, 63, 1, 63), 1, 63) ;
  int right = constrain(map(value, 64, 126, 63, 127), 63, 126) ;
  if (function_graphType[function_id] == 0) {
    displays[display_id].fillRect(1, 26, constrain(value, 1, 126), 12, SSD1306_WHITE) ;
  } else {
    displays[display_id].fillRect(left, 26, right - left + 1, 12, SSD1306_WHITE) ;
  }
  displays[display_id].drawLine(1, 23, 1, 40, SSD1306_WHITE) ;
  displays[display_id].drawLine(126, 23, 126, 40, SSD1306_WHITE) ;
  displays[display_id].drawLine(63, 23, 63, 40, SSD1306_WHITE) ;
  displays[display_id].drawLine(1, 32, 126, 32, SSD1306_WHITE) ;
  displays[display_id].display() ;
}

// ============================================================
// PAGE 2 DISPLAY FUNCTIONS
// ============================================================

void drawLumaKeyDisplay() {
  displays[0].clearDisplay() ;
  displays[0].setTextColor(SSD1306_WHITE) ;
  displays[0].setTextSize(2) ;
  int16_t x1, y1 ; uint16_t w, h ;
  displays[0].getTextBounds("LUMA KEY", 0, 0, &x1, &y1, &w, &h) ;
  displays[0].setCursor((128 - w) / 2, 0) ;
  displays[0].println("LUMA KEY") ;
  int val = constrain(lumaKeyValue, 0, 126) ;
  displays[0].fillRect(1, 26, val, 12, SSD1306_WHITE) ;
  displays[0].drawLine(1, 23, 1, 40, SSD1306_WHITE) ;
  displays[0].drawLine(126, 23, 126, 40, SSD1306_WHITE) ;
  displays[0].drawLine(1, 32, 126, 32, SSD1306_WHITE) ;
  displays[0].drawFastHLine(2, 45, 124, SSD1306_WHITE) ;
  displays[0].setTextSize(1) ;
  displays[0].setCursor(6, 48) ; displays[0].println("LUMA LO") ;
  if (!lumaLowEnabled) { displays[0].fillRect(83, 47, 19, 9, SSD1306_WHITE) ; displays[0].setTextColor(SSD1306_BLACK) ; }
  displays[0].setCursor(84, 48) ; displays[0].println("OFF") ;
  displays[0].setTextColor(SSD1306_WHITE) ;
  if (lumaLowEnabled)  { displays[0].fillRect(107, 47, 14, 9, SSD1306_WHITE) ; displays[0].setTextColor(SSD1306_BLACK) ; }
  displays[0].setCursor(108, 48) ; displays[0].println("ON") ;
  displays[0].setTextColor(SSD1306_WHITE) ;
  displays[0].display() ;
  page2Updated[P2_LUMA] = true ;
}

void drawLumaHighDisplay() {
  displays[1].clearDisplay() ;
  displays[1].setTextColor(SSD1306_WHITE) ;
  displays[1].setTextSize(2) ;
  int16_t x1, y1 ; uint16_t w, h ;
  displays[1].getTextBounds("LUMA HI", 0, 0, &x1, &y1, &w, &h) ;
  displays[1].setCursor((128 - w) / 2, 0) ;
  displays[1].println("LUMA HI") ;
  int val = constrain(lumaHighValue, 0, 126) ;
  displays[1].fillRect(1, 26, val, 12, SSD1306_WHITE) ;
  displays[1].drawLine(1, 23, 1, 40, SSD1306_WHITE) ;
  displays[1].drawLine(126, 23, 126, 40, SSD1306_WHITE) ;
  displays[1].drawLine(1, 32, 126, 32, SSD1306_WHITE) ;
  displays[1].drawFastHLine(2, 45, 124, SSD1306_WHITE) ;
  displays[1].setTextSize(1) ;
  displays[1].setCursor(6, 48) ; displays[1].println("HI CUT") ;
  if (!lumaHighEnabled) { displays[1].fillRect(83, 47, 19, 9, SSD1306_WHITE) ; displays[1].setTextColor(SSD1306_BLACK) ; }
  displays[1].setCursor(84, 48) ; displays[1].println("OFF") ;
  displays[1].setTextColor(SSD1306_WHITE) ;
  if (lumaHighEnabled)  { displays[1].fillRect(107, 47, 14, 9, SSD1306_WHITE) ; displays[1].setTextColor(SSD1306_BLACK) ; }
  displays[1].setCursor(108, 48) ; displays[1].println("ON") ;
  displays[1].setTextColor(SSD1306_WHITE) ;
  displays[1].display() ;
  page2Updated[P2_LUMA_HI] = true ;
}

void drawFadeDisplay() {
  displays[2].clearDisplay() ;
  displays[2].setTextColor(SSD1306_WHITE) ;
  displays[2].setTextSize(2) ;
  int16_t x1, y1 ; uint16_t w, h ;
  displays[2].getTextBounds("FADE", 0, 0, &x1, &y1, &w, &h) ;
  displays[2].setCursor((128 - w) / 2, 0) ;
  displays[2].println("FADE") ;
  int val = constrain(fadeToBlackValue, 0, 126) ;
  displays[2].fillRect(1, 26, val, 12, SSD1306_WHITE) ;
  displays[2].drawLine(1, 23, 1, 40, SSD1306_WHITE) ;
  displays[2].drawLine(126, 23, 126, 40, SSD1306_WHITE) ;
  displays[2].drawLine(1, 32, 126, 32, SSD1306_WHITE) ;
  displays[2].display() ;
  page2Updated[P2_FADE] = true ;
}

void drawLumaSourceDisplay() {
  displays[4].clearDisplay() ;
  displays[4].setTextColor(SSD1306_WHITE) ;
  displays[4].setTextSize(2) ;
  int16_t x1, y1 ; uint16_t w, h ;
  displays[4].getTextBounds("LUMA SRC", 0, 0, &x1, &y1, &w, &h) ;
  displays[4].setCursor((128 - w) / 2, 0) ;
  displays[4].println("LUMA SRC") ;
  displays[4].drawFastHLine(2, 16, 124, SSD1306_WHITE) ;
  // 3 items, 9px rows, y=18-44, bottom divider y=44
  for (int i = 0 ; i < 3 ; i++) {
    int idx = lumaSourceScroll + i ;
    if (idx >= NUM_LUMA_SOURCES) break ;
    int yPos = 18 + (i * 9) ;
    displays[4].setTextSize(1) ;
    if (idx == lumaSourceCursor) {
      displays[4].fillRect(2, yPos - 1, 124, 9, SSD1306_WHITE) ;
      displays[4].setTextColor(SSD1306_BLACK) ;
    } else {
      displays[4].setTextColor(SSD1306_WHITE) ;
    }
    if (idx == lumaSourceSel) {
      displays[4].setCursor(2, yPos) ; displays[4].print(">") ;
      displays[4].setCursor(14, yPos) ;
    } else {
      displays[4].setCursor(6, yPos) ;
    }
    displays[4].println(lumaSourceNames[idx]) ;
  }
  displays[4].setTextColor(SSD1306_WHITE) ;
  displays[4].drawFastHLine(2, 44, 124, SSD1306_WHITE) ;
  displays[4].display() ;
  page2Updated[P2_LUMA_SRC] = true ;
}

// Draw mixer display 8 content — shared by normal draw and page preview
void drawMixerDisplay8Content() {
  displays[8].setTextColor(SSD1306_WHITE) ;
  int16_t x1, y1 ; uint16_t w, h ;

  if (mixerSubMode == 1) {
    displays[8].setTextSize(1) ;
    displays[8].getTextBounds("VIDEO MIXER", 0, 0, &x1, &y1, &w, &h) ;
    displays[8].setCursor((128 - w) / 2, 1) ;
    displays[8].println("VIDEO MIXER") ;
    displays[8].drawFastHLine(2, 11, 124, SSD1306_WHITE) ;

    // A box + source name above bar
    displays[8].fillRect(2, 13, 9, 9, SSD1306_WHITE) ;
    displays[8].setTextColor(SSD1306_BLACK) ;
    displays[8].setCursor(4, 14) ; displays[8].print("A") ;
    displays[8].setTextColor(SSD1306_WHITE) ;
    displays[8].setCursor(14, 14) ; displays[8].println(sourceNames[chASource]) ;

    // Mix bar — drawn from smoothed value
    int sv = (int)mixValueSmoothed ;
    int left  = constrain(map(sv, 1, 63, 1, 63), 1, 63) ;
    int right = constrain(map(sv, 64, 126, 63, 127), 63, 126) ;
    displays[8].fillRect(left, 27, right - left + 1, 6, SSD1306_WHITE) ;
    displays[8].drawLine(1, 24, 1, 34, SSD1306_WHITE) ;
    displays[8].drawLine(126, 24, 126, 34, SSD1306_WHITE) ;
    displays[8].drawLine(63, 24, 63, 34, SSD1306_WHITE) ;
    displays[8].drawLine(1, 30, 126, 30, SSD1306_WHITE) ;

    // Target position marker — same mapping as bar so it aligns at rest
    int targetX ;
    if (mixValue <= 63) {
      targetX = constrain(map(mixValue, 0, 63, 1, 63), 1, 63) ;
    } else {
      targetX = constrain(map(mixValue, 63, 127, 63, 126), 63, 126) ;
    }
    displays[8].drawFastVLine(targetX, 22, 16, SSD1306_WHITE) ;

    // B source name + B box below bar
    displays[8].setTextColor(SSD1306_WHITE) ;
    displays[8].getTextBounds(sourceNames[chBSource], 0, 0, &x1, &y1, &w, &h) ;
    displays[8].setCursor(115 - w, 38) ;
    displays[8].println(sourceNames[chBSource]) ;
    displays[8].fillRect(117, 37, 9, 9, SSD1306_WHITE) ;
    displays[8].setTextColor(SSD1306_BLACK) ;
    displays[8].setCursor(119, 38) ; displays[8].print("B") ;
    displays[8].setTextColor(SSD1306_WHITE) ;
  } else {
    const char* header  = (mixerSubMode == 0) ? "CHANNEL A" : "CHANNEL B" ;
    int selIdx          = (mixerSubMode == 0) ? chASelection : chBSelection ;
    int scrIdx          = (mixerSubMode == 0) ? chAScroll    : chBScroll ;
    int confirmed       = (mixerSubMode == 0) ? chASource    : chBSource ;
    displays[8].setTextSize(1) ;
    displays[8].getTextBounds(header, 0, 0, &x1, &y1, &w, &h) ;
    displays[8].setCursor((128 - w) / 2, 1) ; displays[8].println(header) ;
    displays[8].drawFastHLine(2, 11, 124, SSD1306_WHITE) ;
    // 4 items, moved up 1px: y=13+(i*9), bottom divider at y=50
    for (int i = 0 ; i < 4 ; i++) {
      int idx = scrIdx + i ;
      if (idx >= NUM_SOURCES) break ;
      int yPos = 13 + (i * 9) ;
      if (idx == selIdx) { displays[8].fillRect(2, yPos - 1, 124, 9, SSD1306_WHITE) ; displays[8].setTextColor(SSD1306_BLACK) ; }
      else { displays[8].setTextColor(SSD1306_WHITE) ; }
      if (idx == confirmed) { displays[8].setCursor(2, yPos) ; displays[8].print(">") ; displays[8].setCursor(14, yPos) ; }
      else { displays[8].setCursor(8, yPos) ; }
      displays[8].println(sourceNames[idx]) ;
    }
    displays[8].setTextColor(SSD1306_WHITE) ;
    displays[8].drawFastHLine(2, 48, 124, SSD1306_WHITE) ;
    displays[8].setCursor(2, 56) ; displays[8].println("PUSH ENC TO CONFIRM") ;
  }
}

void drawMixerDisplay() {
  displays[8].clearDisplay() ;
  drawMixerDisplay8Content() ;
  displays[8].display() ;
  page2Updated[P2_MIXER] = true ;
}

// Draw display 8 as it looks on a given page — for seamless page select preview
void previewPageDisplay8(int p) {
  displays[8].clearDisplay() ;
  displays[8].setTextColor(SSD1306_WHITE) ;
  int16_t x1, y1 ; uint16_t w, h ;

  if (p == 0) {
    // Page 0: LINE IN header + volume bar (same as updateVolumeDisplay)
    displays[8].setTextSize(1) ;
    displays[8].getTextBounds("LINE IN", 0, 0, &x1, &y1, &w, &h) ;
    displays[8].setCursor((128 - w) / 2, 1) ; displays[8].println("LINE IN") ;
    int barFill = map(map(function_value[8], 0, 127, 0, 32), 0, 32, 0, 122) ;
    displays[8].drawRect(2, 11, 124, 8, SSD1306_WHITE) ;
    if (barFill > 0) { displays[8].fillRect(3, 12, barFill, 6, SSD1306_WHITE) ; }
  } else if (p == 1) {
    // Page 1: VIDEO OUT header + now playing
    displays[8].setTextSize(1) ;
    displays[8].getTextBounds("VIDEO OUT", 0, 0, &x1, &y1, &w, &h) ;
    displays[8].setCursor((128 - w) / 2, 1) ; displays[8].println("VIDEO OUT") ;
    displays[8].getTextBounds(nowPlaying, 0, 0, &x1, &y1, &w, &h) ;
    displays[8].setCursor(constrain((128 - w) / 2, 2, 128), 10) ; displays[8].println(nowPlaying) ;
    // play/pause icon
    if (isPlaying) { displays[8].fillTriangle(2, 19, 2, 27, 9, 23, SSD1306_WHITE) ; }
    else { displays[8].fillRect(2, 19, 3, 9, SSD1306_WHITE) ; displays[8].fillRect(7, 19, 3, 9, SSD1306_WHITE) ; }
    displays[8].drawFastHLine(2, 29, 124, SSD1306_WHITE) ;
    // clip list
    for (int i = 0 ; i < 3 ; i++) {
      int idx = menuScroll + i ;
      if (idx >= clipCount) break ;
      int yPos = 31 + (i * 11) ;
      if (idx == menuSelection) { displays[8].fillRect(2, yPos - 1, 124, 11, SSD1306_WHITE) ; displays[8].setTextColor(SSD1306_BLACK) ; }
      else { displays[8].setTextColor(SSD1306_WHITE) ; }
      if (idx == clipPos) { displays[8].setCursor(2, yPos) ; displays[8].print(">") ; displays[8].setCursor(11, yPos) ; }
      else { displays[8].setCursor(6, yPos) ; }
      displays[8].println(clipNames[idx]) ;
    }
    displays[8].setTextColor(SSD1306_WHITE) ;
  } else if (p == 2) {
    // Page 2: mixer display
    drawMixerDisplay8Content() ;
  }
  displays[8].display() ;
}

void drawPatchDisplay() {
  displays[5].clearDisplay() ;
  displays[5].setTextColor(SSD1306_WHITE) ;
  displays[5].setTextSize(1) ;
  int16_t x1, y1 ; uint16_t w, h ;
  displays[5].getTextBounds("VIDEO PATCHES", 0, 0, &x1, &y1, &w, &h) ;
  displays[5].setCursor((128 - w) / 2, 1) ;
  displays[5].println("VIDEO PATCHES") ;
  displays[5].drawFastHLine(2, 10, 124, SSD1306_WHITE) ;

  if (patchFlashActive) {
    const char* msg = patchFlashSaved ? "SAVED!" : "LOADED!" ;
    displays[5].setTextSize(2) ;
    displays[5].getTextBounds(msg, 0, 0, &x1, &y1, &w, &h) ;
    displays[5].setCursor((128 - w) / 2, 26) ;
    displays[5].println(msg) ;
    displays[5].display() ;
    page2Updated[P2_PATCH] = true ;
    return ;
  }

  for (int i = 0 ; i < 3 ; i++) {
    int idx  = patchScroll + i ;
    if (idx >= 32) break ;
    int yPos = 13 + (i * 9) ;
    if (idx == patchCursor) {
      displays[5].fillRect(2, yPos - 1, 124, 9, SSD1306_WHITE) ;
      displays[5].setTextColor(SSD1306_BLACK) ;
    } else {
      displays[5].setTextColor(SSD1306_WHITE) ;
    }
    displays[5].drawRect(4, yPos, 7, 7, idx == patchCursor ? SSD1306_BLACK : SSD1306_WHITE) ;
    if (patchFilled[idx]) {
      displays[5].fillRect(5, yPos + 1, 5, 5, idx == patchCursor ? SSD1306_BLACK : SSD1306_WHITE) ;
    }
    displays[5].setCursor(14, yPos) ;
    displays[5].println(patchNames[idx]) ;
  }
  displays[5].setTextColor(SSD1306_WHITE) ;
  displays[5].drawFastHLine(2, 44, 124, SSD1306_WHITE) ;
  displays[5].setCursor(2, 47) ;
  displays[5].println("HOLD=SAVE  TAP=LOAD") ;
  displays[5].display() ;
  page2Updated[P2_PATCH] = true ;
}

void drawAudioPatchDisplay() {
  displays[6].clearDisplay() ;
  displays[6].setTextColor(SSD1306_WHITE) ;
  displays[6].setTextSize(1) ;
  int16_t x1, y1 ; uint16_t w, h ;
  displays[6].getTextBounds("AUDIO PATCHES", 0, 0, &x1, &y1, &w, &h) ;
  displays[6].setCursor((128 - w) / 2, 1) ;
  displays[6].println("AUDIO PATCHES") ;
  displays[6].drawFastHLine(2, 10, 124, SSD1306_WHITE) ;

  if (audioPatchFlashActive) {
    const char* msg = audioPatchFlashSaved ? "SAVED!" : "LOADED!" ;
    displays[6].setTextSize(2) ;
    displays[6].getTextBounds(msg, 0, 0, &x1, &y1, &w, &h) ;
    displays[6].setCursor((128 - w) / 2, 26) ;
    displays[6].println(msg) ;
    displays[6].display() ;
    page2Updated[P2_AUDIO] = true ;
    return ;
  }

  for (int i = 0 ; i < 3 ; i++) {
    int idx = audioPatchScroll + i ;
    if (idx >= 32) break ;
    int yPos = 13 + (i * 9) ;
    if (idx == audioPatchCursor) {
      displays[6].fillRect(2, yPos - 1, 124, 9, SSD1306_WHITE) ;
      displays[6].setTextColor(SSD1306_BLACK) ;
    } else {
      displays[6].setTextColor(SSD1306_WHITE) ;
    }
    displays[6].drawRect(4, yPos, 7, 7, idx == audioPatchCursor ? SSD1306_BLACK : SSD1306_WHITE) ;
    if (audioPatchFilled[idx]) {
      displays[6].fillRect(5, yPos + 1, 5, 5, idx == audioPatchCursor ? SSD1306_BLACK : SSD1306_WHITE) ;
    }
    displays[6].setCursor(14, yPos) ;
    displays[6].println(audioPatchNames[idx]) ;
  }
  displays[6].setTextColor(SSD1306_WHITE) ;
  displays[6].drawFastHLine(2, 44, 124, SSD1306_WHITE) ;
  displays[6].setCursor(2, 47) ;
  displays[6].println("HOLD=SAVE  TAP=LOAD") ;
  displays[6].display() ;
  page2Updated[P2_AUDIO] = true ;
}

// ============================================================
// VIDEO DISPLAY — page 1
// ============================================================

void drawVideoDisplay() {
  displays[8].clearDisplay() ;
  displays[8].setTextSize(1) ;
  displays[8].setTextColor(SSD1306_WHITE) ;
  int16_t x1, y1 ; uint16_t w, h ;
  displays[8].getTextBounds("VIDEO OUT", 0, 0, &x1, &y1, &w, &h) ;
  displays[8].setCursor((128 - w) / 2, 1) ; displays[8].println("VIDEO OUT") ;
  displays[8].getTextBounds(nowPlaying, 0, 0, &x1, &y1, &w, &h) ;
  displays[8].setCursor(constrain((128 - w) / 2, 2, 128), 10) ; displays[8].println(nowPlaying) ;
  if (isPlaying) { displays[8].fillTriangle(2, 19, 2, 27, 9, 23, SSD1306_WHITE) ; }
  else { displays[8].fillRect(2, 19, 3, 9, SSD1306_WHITE) ; displays[8].fillRect(7, 19, 3, 9, SSD1306_WHITE) ; }
  if (isLooping) { displays[8].fillRect(13, 19, 9, 9, SSD1306_WHITE) ; displays[8].setTextColor(SSD1306_BLACK) ; }
  displays[8].setCursor(15, 20) ; displays[8].println("L") ;
  displays[8].setTextColor(SSD1306_WHITE) ;
  int curSecs = constrain(progressPos, 0, progressTotal) ;
  char timeBuf[6] ; snprintf(timeBuf, 6, "%d:%02d", curSecs / 60, curSecs % 60) ;
  displays[8].getTextBounds(timeBuf, 0, 0, &x1, &y1, &w, &h) ;
  int timeX = 125 - w ;
  displays[8].setCursor(timeX, 21) ; displays[8].println(timeBuf) ;
  int barLeft = 24, barRight = timeX - 3, barWidth = barRight - barLeft ;
  displays[8].drawRect(barLeft, 20, barWidth, 8, SSD1306_WHITE) ;
  if (progressTotal > 0 && barWidth > 2) {
    int fillW = constrain((int)((float)(barWidth - 2) * progressPos / progressTotal), 0, barWidth - 2) ;
    if (fillW > 0) displays[8].fillRect(barLeft + 1, 21, fillW, 6, SSD1306_WHITE) ;
  }
  displays[8].drawFastHLine(2, 29, 124, SSD1306_WHITE) ;
  for (int i = 0 ; i < 3 ; i++) {
    int idx = menuScroll + i ;
    if (idx >= clipCount) break ;
    int yPos = 31 + (i * 11) ;
    if (idx == menuSelection) { displays[8].fillRect(2, yPos - 1, 124, 11, SSD1306_WHITE) ; displays[8].setTextColor(SSD1306_BLACK) ; }
    else { displays[8].setTextColor(SSD1306_WHITE) ; }
    if (idx == clipPos) { displays[8].setCursor(2, yPos) ; displays[8].print(">") ; displays[8].setCursor(11, yPos) ; }
    else { displays[8].setCursor(6, yPos) ; }
    displays[8].println(clipNames[idx]) ;
  }
  displays[8].setTextColor(SSD1306_WHITE) ;
  displays[8].display() ;
  videoDisplayUpdated = true ;
}

// ============================================================
// SPECTRUM + VOLUME + FPS
// ============================================================

void updateVolumeDisplay() {
  displays[8].fillRect(2, 0, 124, 22, SSD1306_BLACK) ;
  displays[8].setTextSize(1) ; displays[8].setTextColor(SSD1306_WHITE) ;
  int16_t x1, y1 ; uint16_t w, h ;
  displays[8].getTextBounds("LINE IN", 0, 0, &x1, &y1, &w, &h) ;
  displays[8].setCursor((128 - w) / 2, 1) ; displays[8].println("LINE IN") ;
  int barFill = map(map(function_value[8], 0, 127, 0, 32), 0, 32, 0, 122) ;
  displays[8].drawRect(2, 11, 124, 8, SSD1306_WHITE) ;
  if (barFill > 0) { displays[8].fillRect(3, 12, barFill, 6, SSD1306_WHITE) ; }
  displays[8].display() ;
}

void drawSpectrum() {
  if (page == 0 && !enc8HeldLong && pendingPage < 0) {
    if (fft.available()) {
      displays[8].fillRect(0, 22, 128, 42, SSD1306_BLACK) ;
      unsigned long now = millis() ;
      for (int i = 0 ; i < 32 ; i++) {
        float level = fft.read((i * 4) + 1) ;
        level *= 1.0f + (float)i * 0.18f ;
        level *= map(function_value[8], 0, 127, 0, 32) / 25.0f ;
        smoothedLevels[i] += (level - smoothedLevels[i]) * (level > smoothedLevels[i] ? ATTACK : DECAY) ;
        if (smoothedLevels[i] >= peakLevels[i]) { peakLevels[i] = smoothedLevels[i] ; peakHoldStart[i] = now ; }
        else if (now - peakHoldStart[i] > PEAK_HOLD_MS) { peakLevels[i] = max(0.0f, peakLevels[i] - PEAK_DECAY) ; }
        int barHeight  = constrain((int)(smoothedLevels[i] * 38 * 8), 0, 38) ;
        int peakHeight = constrain((int)(peakLevels[i]    * 38 * 8), 0, 38) ;
        if (barHeight > 0) { displays[8].fillRect(1 + i * 4, 63 - barHeight, 3, barHeight, SSD1306_WHITE) ; }
        if (peakHeight > barHeight && peakHeight > 0) { displays[8].drawFastHLine(1 + i * 4, 63 - peakHeight, 3, SSD1306_WHITE) ; }
      }
      displays[8].display() ;
    }
  }
}

// ============================================================
// MIDI
// ============================================================

void initializeAllMidiControls() {
  usbMIDI.sendControlChange(16, 64, 1) ; usbMIDI.send_now() ;
  for(byte cc = 17 ; cc <= 127 ; cc++) {
    usbMIDI.sendControlChange(cc, 63, 1) ;
    if (cc % 8 == 0) { usbMIDI.send_now() ; delayMicroseconds(500) ; }
  }
  usbMIDI.send_now() ;
}

void sweepAndLatch() {
  usbMIDI.sendControlChange(91, 127, 1) ; usbMIDI.send_now() ;
  unsigned long t = millis() ; while(millis()-t < 50) { usbMIDI.read() ; }
  for (int i = 0 ; i < 18 ; i++) {
    if (cc_encoder[i] == 0) continue ;
    int sv = (i == 8) ? map(function_value[i], 0, 127, 0, 32) : function_value[i] ;
    usbMIDI.sendControlChange(cc_encoder[i], sv, 1) ;
    usbMIDI.send_now() ;
    usbMIDI.read() ;
    delayMicroseconds(500) ;
  }
  t = millis() ; while(millis()-t < 50) { usbMIDI.read() ; }
  usbMIDI.sendControlChange(91, 0, 1) ; usbMIDI.send_now() ;
}

void scanPatches() {
  AudioNoInterrupts() ;
  for (int i = 0 ; i < 32 ; i++) {
    snprintf(patchNames[i], 10, "PATCH %02d", i + 1) ;
    patchFilled[i] = false ;
  }
  File root = SD.open("/") ;
  if (!root) return ;
  while (true) {
    File f = root.openNextFile() ;
    if (!f) break ;
    char fname[32] ;
    strncpy(fname, f.name(), 31) ; fname[31] = '\0' ;
    f.close() ;
    for (int j = 0 ; fname[j] ; j++) fname[j] = toupper(fname[j]) ;
    int flen = strlen(fname) ;
    if (flen < 9) continue ;
    if (strncmp(fname, "PATCH", 5) != 0) continue ;
    if (strcmp(fname + flen - 4, ".TXT") != 0) continue ;
    // extract slot number
    char numBuf[3] = { fname[5], fname[6], '\0' } ;
    int slot = atoi(numBuf) - 1 ;
    if (slot < 0 || slot >= 32) continue ;
    patchFilled[slot] = true ;
    // extract suffix if present
    if (flen - 4 > 7) {
      char suffix[10] ;
      int slen = min((int)(flen - 4 - 7), 8) ;
      strncpy(suffix, fname + 7, slen) ;
      suffix[slen] = '\0' ;
      strncpy(patchNames[slot], suffix, 9) ;
    }
  }
  root.close() ;
  AudioInterrupts() ;
}

void scanAudioPatches() {
  AudioNoInterrupts() ;
  for (int i = 0 ; i < 32 ; i++) {
    snprintf(audioPatchNames[i], 10, "AUDIO %02d", i + 1) ;
    audioPatchFilled[i] = false ;
  }
  File root = SD.open("/") ;
  if (!root) { AudioInterrupts() ; return ; }
  while (true) {
    File f = root.openNextFile() ;
    if (!f) break ;
    char fname[32] ;
    strncpy(fname, f.name(), 31) ; fname[31] = '\0' ;
    f.close() ;
    for (int j = 0 ; fname[j] ; j++) fname[j] = toupper(fname[j]) ;
    int flen = strlen(fname) ;
    if (flen < 9) continue ;
    if (strncmp(fname, "AUDIO", 5) != 0) continue ;
    if (strcmp(fname + flen - 4, ".TXT") != 0) continue ;
    char numBuf[3] = { fname[5], fname[6], '\0' } ;
    int slot = atoi(numBuf) - 1 ;
    if (slot < 0 || slot >= 32) continue ;
    audioPatchFilled[slot] = true ;
  }
  root.close() ;
  AudioInterrupts() ;
}

/**
 * Save current state to a patch slot on SD card.
 * Patch files are plain text with key=value pairs.
 * Called when user holds encoder 5 button on page 2.
 *
 * @param slot Patch slot number (0-31)
 */
void savePatch(int slot) {
  AudioNoInterrupts() ;  // Pause audio during SD access
  char fname[24] ;
  snprintf(fname, 24, "/PATCH%02d.TXT", slot + 1) ;

  // Remove existing file to ensure clean write
  if (SD.exists(fname)) SD.remove(fname) ;
  File f = SD.open(fname, FILE_WRITE) ;
  if (!f) return ;

  // Save encoder values (18 parameters across 2 pages)
  for (int i = 0 ; i < 18 ; i++) { f.print("enc") ; f.print(i) ; f.print("=") ; f.println(function_value[i]) ; }

  // Save toggle states (8 on/off controls)
  for (int i = 0 ; i < 8 ; i++)  { f.print("ctrl") ; f.print(i) ; f.print("=") ; f.println(control[i] ? 1 : 0) ; }

  // Save function multiplier states (8 encoders × 4 levels each)
  for (int i = 0 ; i < 8 ; i++)  { f.print("func") ; f.print(i) ; f.print("=") ; f.println(function[i]) ; }
  f.print("react=")     ; f.println(react_band) ;
  f.print("lumaKey=")   ; f.println(lumaKeyValue) ;
  f.print("lumaHigh=")  ; f.println(lumaHighValue) ;
  f.print("lumaLowEn=") ; f.println(lumaLowEnabled ? 1 : 0) ;
  f.print("lumaHiEn=")  ; f.println(lumaHighEnabled ? 1 : 0) ;
  f.print("mix=")       ; f.println(mixValue) ;
  f.print("chA=")       ; f.println(chASource) ;
  f.print("chB=")       ; f.println(chBSource) ;
  f.print("lumaSrc=")   ; f.println(lumaSourceSel) ;
  f.print("mixMode=")   ; f.println(mixerSubMode) ;
  f.print("clip=")      ; f.println(clipPos) ;
  f.print("loop=")      ; f.println(isLooping ? 1 : 0) ;
  f.print("srcHue=")    ; f.println(srcHue) ;
  f.print("srcSat=")    ; f.println(srcSat) ;
  f.print("srcVal=")    ; f.println(srcVal) ;
  f.print("srcShader=") ; f.println(srcShader) ;
  f.close() ;
  patchFilled[slot] = true ;
  snprintf(patchNames[slot], 10, "PATCH %02d", slot + 1) ;
  AudioInterrupts() ;
}

/**
 * Load a patch from SD card and apply to current state.
 * Parses the key=value pairs and updates all parameters.
 * Sends MIDI and serial commands to sync autowaaave and autoclip.
 * Called when user taps encoder 5 button on page 2 (if slot is filled).
 *
 * @param slot Patch slot number (0-31)
 */
void loadPatch(int slot) {
  char fname[24] ;
  snprintf(fname, 24, "/PATCH%02d.TXT", slot + 1) ;
  AudioNoInterrupts() ;  // Pause audio during SD access

  if (!SD.exists(fname)) { AudioInterrupts() ; return ; }
  File f = SD.open(fname) ;
  if (!f) { AudioInterrupts() ; return ; }

  // Parse each line of the patch file
  while (f.available()) {
    char line[32] ;
    int len = 0 ;
    while (f.available() && len < 31) {
      char c = f.read() ;
      if (c == '\n') break ;
      if (c != '\r') line[len++] = c ;
    }
    line[len] = '\0' ;
    char* eq = strchr(line, '=') ;
    if (!eq) continue ;
    *eq = '\0' ;
    char* key = line ;
    int val = atoi(eq + 1) ;
    if (strncmp(key, "enc", 3) == 0) {
      int i = atoi(key + 3) ;
      if (i >= 0 && i < 18) function_value[i] = val ;
    } else if (strncmp(key, "ctrl", 4) == 0) {
      int i = atoi(key + 4) ;
      if (i >= 0 && i < 8) control[i] = (bool)val ;
    } else if (strncmp(key, "func", 4) == 0) {
      int i = atoi(key + 4) ;
      if (i >= 0 && i < 8) function[i] = val ;
    }
    else if (strcmp(key, "react") == 0)     { react_band      = val ; }
    else if (strcmp(key, "lumaKey") == 0)   { lumaKeyValue     = val ; }
    else if (strcmp(key, "lumaHigh") == 0)  { lumaHighValue    = val ; }
    else if (strcmp(key, "lumaLowEn") == 0) { lumaLowEnabled   = (bool)val ; }
    else if (strcmp(key, "lumaHiEn") == 0)  { lumaHighEnabled  = (bool)val ; }
    else if (strcmp(key, "mix") == 0)       { mixValue         = val ; mixValueSmoothed = val ; }
    else if (strcmp(key, "chA") == 0)       { chASource        = val ; chASelection = val ; }
    else if (strcmp(key, "chB") == 0)       { chBSource        = val ; chBSelection = val ; }
    else if (strcmp(key, "lumaSrc") == 0)   { lumaSourceSel    = val ; lumaSourceCursor = val ; }
    else if (strcmp(key, "mixMode") == 0)   { mixerSubMode     = val ; }
    else if (strcmp(key, "clip") == 0)      { clipPos          = val ; }
    else if (strcmp(key, "loop") == 0)      { isLooping = (bool)val ; }
    else if (strcmp(key, "srcHue") == 0)    { srcHue           = val ; }
    else if (strcmp(key, "srcSat") == 0)    { srcSat           = val ; }
    else if (strcmp(key, "srcVal") == 0)    { srcVal           = val ; }
    else if (strcmp(key, "srcShader") == 0) { srcShader        = val ; }
  }
  f.close() ;
  AudioInterrupts() ;
  char cmdBuf[24] ;
  snprintf(cmdBuf, 24, "LUMA:%d",         lumaLowEnabled ? lumaKeyValue : 0) ; queueVideoCommand(cmdBuf) ;
  snprintf(cmdBuf, 24, "LUMA_HIGH:%d",    lumaHighValue) ;                     queueVideoCommand(cmdBuf) ;
  snprintf(cmdBuf, 24, "LUMA_HIGH_EN:%d", lumaHighEnabled ? 1 : 0) ;           queueVideoCommand(cmdBuf) ;
  snprintf(cmdBuf, 24, "LUMA_SRC:%d",     lumaSourceSel) ;                     queueVideoCommand(cmdBuf) ;
  snprintf(cmdBuf, 24, "MIX:%d",          mixValue) ;                          queueVideoCommand(cmdBuf) ;
  snprintf(cmdBuf, 24, "CH_A:%d",         chASource) ;                         queueVideoCommand(cmdBuf) ;
  snprintf(cmdBuf, 24, "CH_B:%d",         chBSource) ;                         queueVideoCommand(cmdBuf) ;
  queueVideoCommand(isLooping ? "LOOP_ON" : "LOOP_OFF") ;
  snprintf(cmdBuf, 24, "PLAY:%d",         clipPos) ;                           queueVideoCommand(cmdBuf) ;
  patchLoadPending = true ;
  patchLoadStep    = 0 ;
  patchLoadTimer   = millis() ;
}

void executePatchLoad() {
  if (!patchLoadPending) return ;
  if (millis() - patchLoadTimer < 20) return ;
  patchLoadTimer = millis() ;

  if (patchLoadStep == 0) {
    Serial.println("PATCH: CC91 HIGH") ;
    usbMIDI.sendControlChange(91, 127, 1) ; usbMIDI.send_now() ;
    patchLoadStep++ ; return ;
  }
  if (patchLoadStep >= 1 && patchLoadStep <= 18) {
    int i = patchLoadStep - 1 ;
    if (cc_encoder[i] != 0) {
      int sv = (i == 8) ? map(function_value[i], 0, 127, 0, 32) : function_value[i] ;
      usbMIDI.sendControlChange(cc_encoder[i], sv, 1) ; usbMIDI.send_now() ;
    }
    patchLoadStep++ ; return ;
  }
  if (patchLoadStep >= 19 && patchLoadStep <= 26) {
    int i = patchLoadStep - 19 ;
    if (i != 7) {
      int ccVal = control[i] ? 127 : 0 ;
      Serial.print("CTRL ") ; Serial.print(i) ; 
      Serial.print(" CC") ; Serial.print(cc_control[i]) ;
      Serial.print(" = ") ; Serial.println(ccVal) ;
      usbMIDI.sendControlChange(cc_control[i], ccVal, 1) ; usbMIDI.send_now() ;
    }
    patchLoadStep++ ; return ;
  }
  if (patchLoadStep >= 27 && patchLoadStep <= 34) {
    int j = patchLoadStep - 27 ;
    for (int m = 1 ; m <= 3 ; m++) { usbMIDI.sendControlChange(16 + (16 * m) + j, 0, 1) ; usbMIDI.send_now() ; }
    if (function[j] > 0) {
      usbMIDI.sendControlChange(16 + (16 * function[j]) + j, 127, 1) ;
      usbMIDI.send_now() ;
    }
    patchLoadStep++ ; return ;
  }
  if (patchLoadStep == 35) {
    for (int i = 0 ; i < 3 ; i++) { usbMIDI.sendControlChange(cc_reactive[i], 0, 1) ; usbMIDI.send_now() ; }
    if (react_band >= 0) { usbMIDI.sendControlChange(cc_reactive[react_band], 127, 1) ; usbMIDI.send_now() ; }
    patchLoadStep++ ; return ;
  }
  if (patchLoadStep == 36) {
    Serial.println("PATCH: CC91 LOW") ;
    usbMIDI.sendControlChange(91, 0, 1) ; usbMIDI.send_now() ;
    patchLoadStep++ ; return ;
  }
  if (patchLoadStep == 37) {
    patchLoadPending      = false ;
    patchFlashActive      = true ;
    patchFlashSaved       = false ;
    patchFlashTimer       = millis() ;
    page2Updated[P2_PATCH]     = false ;
    page2Updated[P2_LUMA]      = false ;
    page2Updated[P2_LUMA_HI]  = false ;
    page2Updated[P2_LUMA_SRC]   = false ;
    page2Updated[P2_MIXER]     = false ;
  }
}

// ============================================================
// SETUP
// ============================================================
// Initializes all hardware: serial, audio, GPIOs, displays, SD card.
// Shows boot animation on OLEDs while waiting for autowaaave to signal ready.

void setup() {
  // USB Serial for communication with video_bridge on autowaaave
  Serial.begin(115200) ;

  // Teensy Audio Library setup for FFT spectrum analysis
  AudioMemory(32) ;           // Allocate audio buffer blocks
  audioShield.enable() ;      // Power on SGTL5000 codec
  audioShield.inputSelect(AUDIO_INPUT_LINEIN) ;  // Use line-in (not mic)
  audioShield.lineInLevel(3) ;// Input sensitivity (0-15, lower=hotter)

  Serial.println("Audio Setup") ;
  pinMode(12, INPUT) ;
  pinMode(BUTTON_DATA, INPUT) ;
  pinMode(BUTTON_CLOCK, OUTPUT) ;
  pinMode(BUTTON_LOAD, OUTPUT) ;
  digitalWrite(BUTTON_CLOCK, LOW) ; digitalWrite(BUTTON_CLOCK, HIGH) ;
  readButtons() ; readButtons() ;
  for(int i = 0 ; i < 12 ; i++) { button[i] = 0 ; }
  pinMode(LED_DATA, OUTPUT) ;
  pinMode(LED_CLOCK, OUTPUT) ;
  pinMode(LED_LATCH, OUTPUT) ;
  setLEDs(0x00) ; delay(50) ; setLEDs(0xFF) ; delay(100) ; setLEDs(0x00) ;
  for (int i = 0 ; i < 9 ; i++) { pinMode(csPins[i], OUTPUT) ; digitalWrite(csPins[i], HIGH) ; }
  pinMode(29, OUTPUT) ;
  digitalWrite(29, LOW) ; delay(250) ; digitalWrite(29, HIGH) ; delay(250) ;
  for (int i = 0 ; i < 9 ; i++) {
    displays[i].begin(SSD1306_SWITCHCAPVCC, 0, false, true) ;
    displays[i].ssd1306_command(0x00) ; displays[i].ssd1306_command(0x11) ;
    displays[i].clearDisplay() ; displays[i].display() ;
    updateLabel(i, boot_label[i], 4) ;
  }
  delay(50) ;
  SD.begin(SD_CS) ;
  scanPatches() ;
  scanAudioPatches() ;
}

unsigned long boot_flag = millis() ;

// ============================================================
// HELPERS
// ============================================================

void invalidateAllDisplays() {
  for(int i = 0 ; i < 9 ; i++) { labelUpdated[i] = false ; graphUpdated[i] = false ; }
  for(int i = 0 ; i < 4 ; i++) { controlUpdated[i] = false ; functionUpdated[i] = false ; }
  videoDisplayUpdated  = false ;
  for(int i = 0 ; i < P2_COUNT ; i++) { page2Updated[i] = false ; }
  for(int i = 0 ; i < 2 ; i++) { page2ClearUpdated[i] = false ; }
}

void switchToPage(int newPage) {
  if (page == 2) { clearScreen(4) ; }
  page = newPage ;
  invalidateAllDisplays() ;
  // Reset encoder accumulators and resync positions on page switch
  enc0Accum = 0 ;
  enc1Accum = 0 ;
  enc4Accum = 0 ;
  enc8Accum = 0 ;
  lastEncPos[0]  = encoders[0]->read() ;
  lastEncPos[1]  = encoders[1]->read() ;
  lastEncPos[4]  = encoders[4]->read() ;
  lastEncPos[8]  = encoders[8]->read() ;
  lastEncPos[17] = encoders[8]->read() ;
  // Resync scroll positions to current selections
  chAScroll = constrain(chASelection - 1, 0, max(0, NUM_SOURCES - 3)) ;
  chBScroll = constrain(chBSelection - 1, 0, max(0, NUM_SOURCES - 3)) ;
  lumaSourceScroll = constrain(lumaSourceCursor - 1, 0, max(0, NUM_LUMA_SOURCES - 3)) ;
  for(int i = 0 ; i < 8 ; i++) { LED_ON[i] = false ; }
  for(int i = 0 ; i < 4 ; i++) { function_flash[i] = 0 ; }
  if (page == 0) {
    LED_ON[LED_6] = (react_band == 0) ;
    LED_ON[LED_7] = (react_band == 1) ;
    LED_ON[LED_8] = (react_band == 2) ;
  } else if (page == 1) {
    labelUpdated[8] = true ;
    LED_ON[LED_5] = true ;
    LED_ON[LED_6] = isLooping ;
    LED_ON[LED_7] = isPlaying ;
  } else if (page == 2) {
    labelUpdated[8] = true ;
    mixerSubMode = 1 ;
    LED_ON[LED_5] = true ;
    LED_ON[LED_6] = false ;
    LED_ON[LED_7] = true ;
    LED_ON[LED_8] = false ;
    clearScreen(2) ; clearScreen(3) ; clearScreen(7) ;
    enc2Accum = 0 ;
    lastEncPos[2] = encoders[2]->read() ;
    enc5Accum = 0 ;
    lastEncPos[5] = encoders[5]->read() ;
    enc6Accum = 0 ;
    lastEncPos[6] = encoders[6]->read() ;
    for(int i = 0 ; i < 9 ; i++) { labelUpdated[i] = true ; graphUpdated[i] = true ; }
    for(int i = 0 ; i < 4 ; i++) { controlUpdated[i] = true ; functionUpdated[i] = true ; }
  }
}

// ============================================================
// MAIN LOOP
// Runs at ~30fps, handles:
//   - Serial commands from autowaaave/autoclip
//   - Button/encoder input reading
//   - MIDI output to autowaaave
//   - Display updates (one per loop to avoid audio starvation)
// ============================================================

void loop() {
  checkSerial() ;
  flushVideoQueue() ;
  usbMIDI.read() ;

  unsigned long nowMicros = micros() ;
  unsigned long delta = nowMicros - lastFrameTime ;
  static int fps_calc_counter = 0 ;
  if (++fps_calc_counter >= 10) {
    fps_calc_counter = 0 ;
    if (delta > 0) {
      currentFPS = 10000000.0f / delta ;
      smoothedFPS = (smoothedFPS * FPS_SMOOTH) + (currentFPS * (1.0f - FPS_SMOOTH)) ;
    }
    lastFrameTime = nowMicros ;
  }

  // BOOT
  if (booting && !booted) {
    if (scroll_LEDs) {
      for(int i = 0 ; i < 8 ; i++) {
        checkSerial() ;
        if (autowaaaveReady) break ;
        digitalWrite(25, LOW) ; shiftOut(22, 24, MSBFIRST, 1 << i) ; digitalWrite(25, HIGH) ; delay(75) ;
      }
      if ((autowaaaveReady || (millis() - boot_flag) > (1000 * boot_time_seconds)) && !booted) {
        scroll_LEDs = false ; booting = false ;
        digitalWrite(LED_LATCH, LOW) ; delay(100) ; setLEDs(0xFF) ;
        digitalWrite(LED_LATCH, HIGH) ; delay(100) ; setLEDs(0x00) ;
        booted = true ;
        initializeAllMidiControls() ;
        for (int i = 0 ; i < 9 ; i++) { clearScreen(i) ; }
        Serial.println("Boot finalized") ;
      }
    }
  }

  if (!booting && booted) {

    if (!midiInitialized) {
      initializeAllMidiControls() ;
      unsigned long t = millis() ; while(millis()-t < 500) { usbMIDI.read() ; }
      sweepAndLatch() ;
      midiInitialized = true ;
    }

    if (millis() - lastMidiWatchdog > MIDI_WATCHDOG_MS) {
      lastMidiWatchdog = millis() ;
      usbMIDI.read() ; usbMIDI.sendControlChange(0, 0, 1) ; usbMIDI.send_now() ;
    }

    // DEBOUNCE
    static unsigned long lastButtonRead = 0 ;
    if (micros() - lastButtonRead > 5000) {
      uint16_t rawButtons = readButtons() ;
      lastButtonRead = micros() ;
      if (rawButtons != lastRawButton) { lastChangeTime = millis() ; lastRawButton = rawButtons ; }
      if (millis() - lastChangeTime > DEBOUNCE_MS) { stableButtons = rawButtons ; }
    }

    uint16_t mask     = 0x0FFF ;
    uint16_t pressed  = (~stableButtons &  lastButtons) & mask ;
    uint16_t released = ( stableButtons & ~lastButtons) & mask ;
    held              = (~stableButtons & ~lastButtons) & mask ;

    byte ledByte = 0 ;

    // Flash timer
    if (millis() - pageFlashTimer > PAGE_FLASH_MS) {
      pageFlashTimer = millis() ;
      pageFlashState = !pageFlashState ;
    }

    // ==================== PAGE SWITCHING ====================

    if (pressed & (1 << BTN_MENU)) {
      menuPressTime = millis() ;
      enc8HeldLong  = false ;
      pendingPage   = -1 ;
      pageSwitched  = false ;
      ledOverride   = false ;
      for(int i = 0 ; i < 8 ; i++) { savedLED[i] = LED_ON[i] ; }
    }

    if (held & (1 << BTN_MENU)) {
      unsigned long heldMs = millis() - menuPressTime ;

      // Ring buttons always work while enc8 held — no hold time requirement
      if (pressed & (1 << BTN_LOW)) {
        int prev = pendingPage ; pendingPage = 0 ;
        if (prev != pendingPage) { previewPageDisplay8(pendingPage) ; suppressDisplay8Draw = true ; }
      }
      if (pressed & (1 << BTN_MID)) {
        int prev = pendingPage ; pendingPage = 1 ;
        if (prev != pendingPage) { previewPageDisplay8(pendingPage) ; suppressDisplay8Draw = true ; }
      }
      if (pressed & (1 << BTN_HI)) {
        int prev = pendingPage ; pendingPage = 2 ;
        if (prev != pendingPage) { previewPageDisplay8(pendingPage) ; suppressDisplay8Draw = true ; }
      }

      if (heldMs >= PAGE_HOLD_MS && !enc8HeldLong) {
        enc8HeldLong = true ;
        ledOverride  = true ;
        if (pendingPage < 0) { previewPageDisplay8(page) ; suppressDisplay8Draw = true ; }
      }
    }

    if (released & (1 << BTN_MENU)) {

      suppressDisplay8Draw = false ;
      bool wasHeldLong = enc8HeldLong ;

      if (pendingPage >= 0) {
        // Page switch always happens if a ring button was pressed, regardless of hold time
        if (pendingPage != page) { switchToPage(pendingPage) ; }
        pageSwitched = true ;
      } else if (!wasHeldLong) {
        // Short press (not held long, no ring button) — do page action
        if (page == 1) {
          if (menuSelection == clipPos) {
            isPlaying = !isPlaying ; LED_ON[LED_7] = isPlaying ;
            sendVideoCommand(isPlaying ? "PLAY" : "PAUSE") ;
          } else {
            isPlaying = true ; LED_ON[LED_7] = true ;
            char cmdBuf[16] ; snprintf(cmdBuf, 16, "PLAY:%d", menuSelection) ;
            sendVideoCommand(cmdBuf) ;
          }
          videoDisplayUpdated = false ;
        } else if (page == 2) {
          if (mixerSubMode == 0) {
            chASource = chASelection ;
            char cmdBuf[16] ; snprintf(cmdBuf, 16, "CH_A:%d", chASource) ;
            sendVideoCommand(cmdBuf) ; page2Updated[P2_MIXER] = false ;
          } else if (mixerSubMode == 2) {
            chBSource = chBSelection ;
            char cmdBuf[16] ; snprintf(cmdBuf, 16, "CH_B:%d", chBSource) ;
            sendVideoCommand(cmdBuf) ; page2Updated[P2_MIXER] = false ;
          }
        }
      }
      // else: held long but no ring button — just restore

      // Restore LEDs and display if no page switch
      if (!pageSwitched) {
        for(int i = 0 ; i < 8 ; i++) { LED_ON[i] = savedLED[i] ; }
        if (page == 0) { graphUpdated[8] = false ; }
        else if (page == 1) { videoDisplayUpdated = false ; }
        else if (page == 2) { page2Updated[P2_MIXER] = false ; }
      }

      enc8HeldLong = false ;
      ledOverride  = false ;
      pendingPage  = -1 ;
      pageSwitched = false ;
    }

    // ==================== PAGE-SPECIFIC CONTROLS ====================
    // Guard with !enc8HeldLong for short-press actions only
    // Ring button controls on each page are separate from page-select ring buttons

    if (!enc8HeldLong) {

      // PAGE 1 VIDEO CONTROLS
      if (page == 1) {
        if (released & (1 << BTN_LOW)) {
          isLooping = !isLooping ; LED_ON[LED_6] = isLooping ;
          sendVideoCommand(isLooping ? "LOOP_ON" : "LOOP_OFF") ;
          videoDisplayUpdated = false ;
        }
        if (released & (1 << BTN_MID)) {
          if (menuSelection == clipPos) {
            isPlaying = !isPlaying ; LED_ON[LED_7] = isPlaying ;
            sendVideoCommand(isPlaying ? "PLAY" : "PAUSE") ;
          } else {
            isPlaying = true ; LED_ON[LED_7] = true ;
            char cmdBuf[16] ; snprintf(cmdBuf, 16, "PLAY:%d", menuSelection) ;
            sendVideoCommand(cmdBuf) ;
          }
          videoDisplayUpdated = false ;
        }
        if (released & (1 << BTN_HI)) {
          sendVideoCommand("NEXT") ; LED_ON[LED_8] = true ; skipLedTime = millis() ;
        }
        if (LED_ON[LED_8] && millis() - skipLedTime > SKIP_LED_MS) { LED_ON[LED_8] = false ; }
      }

      // PAGE 0 REACTIVE
      if (page == 0) {
        for(int i = 0 ; i < 3 ; i++) {
          if (released & (1 << (BTN_LOW + i))) {
            for(int ii = 0 ; ii < 3 ; ii++) { usbMIDI.sendControlChange(cc_reactive[ii], 0, 1) ; usbMIDI.send_now() ; }
            react_band = (react_band == i) ? -1 : i ;
            if (react_band >= 0) { usbMIDI.sendControlChange(cc_reactive[react_band], 127, 1) ; usbMIDI.send_now() ; }
          }
        }
        LED_ON[LED_6] = (react_band == 0) ;
        LED_ON[LED_7] = (react_band == 1) ;
        LED_ON[LED_8] = (react_band == 2) ;
      }

      // PAGE 2 MIXER RING BUTTONS + CONTROLS
      if (page == 2) {
        if (released & (1 << BTN_LOW)) {
          mixerSubMode = 0 ;
          LED_ON[LED_6] = true ; LED_ON[LED_7] = false ; LED_ON[LED_8] = false ;
          page2Updated[P2_MIXER] = false ;
        }
        if (released & (1 << BTN_MID)) {
          mixerSubMode = 1 ;
          LED_ON[LED_6] = false ; LED_ON[LED_7] = true ; LED_ON[LED_8] = false ;
          page2Updated[P2_MIXER] = false ;
        }
        if (released & (1 << BTN_HI)) {
          mixerSubMode = 2 ;
          LED_ON[LED_6] = false ; LED_ON[LED_7] = false ; LED_ON[LED_8] = true ;
          page2Updated[P2_MIXER] = false ;
        }
        // BTN_3 = enc2 push — snap fade to nearest extreme (0 or 127)
        if (released & (1 << BTN_3)) {
          fadeToBlackValue = (fadeToBlackValue > 63) ? 0 : 127 ;
          usbMIDI.sendControlChange(2, fadeToBlackValue, 1) ; usbMIDI.send_now() ;
          page2Updated[P2_FADE] = false ;
        }
        // BTN_1 = toggle luma low on/off
        if (released & (1 << BTN_1)) {
          lumaLowEnabled = !lumaLowEnabled ;
          char cmdBuf[24] ;
          snprintf(cmdBuf, 24, "LUMA:%d", lumaLowEnabled ? lumaKeyValue : 0) ;
          sendVideoCommand(cmdBuf) ;
          page2Updated[P2_LUMA] = false ;
        }
        // BTN_2 = toggle luma high on/off
        if (released & (1 << BTN_2)) {
          lumaHighEnabled = !lumaHighEnabled ;
          char cmdBuf[24] ;
          snprintf(cmdBuf, 24, "LUMA_HIGH_EN:%d", lumaHighEnabled ? 1 : 0) ;
          sendVideoCommand(cmdBuf) ;
          page2Updated[P2_LUMA_HI] = false ;
        }
        // BTN_4 = confirm luma source selection
        if (released & (1 << BTN_5)) {
          lumaSourceSel = lumaSourceCursor ;
          char cmdBuf[16] ; snprintf(cmdBuf, 16, "LUMA_SRC:%d", lumaSourceSel) ;
          sendVideoCommand(cmdBuf) ; page2Updated[P2_LUMA_SRC] = false ;
        }
        // BTN_6 = enc5 push — tap=load, hold=save
        if (pressed & (1 << BTN_6)) {
          btn6PressTime = millis() ;
          btn6HeldLong  = false ;
        }
        if (held & (1 << BTN_6)) {
          if (millis() - btn6PressTime >= PATCH_HOLD_MS && !btn6HeldLong) {
            btn6HeldLong     = true ;
            savePatch(patchCursor) ;
            patchFlashActive = true ;
            patchFlashSaved  = true ;
            patchFlashTimer  = millis() ;
            page2Updated[P2_PATCH] = false ;
          }
        }
        if (released & (1 << BTN_6)) {
          if (!btn6HeldLong) {
            if (patchFilled[patchCursor]) {
              loadPatch(patchCursor) ;
            }
          }
          btn6HeldLong = false ;
        }
        // BTN_7 = enc6 push — tap=load, hold=save audio patch
        if (pressed & (1 << BTN_7)) {
          btn7PressTime = millis() ;
          btn7HeldLong  = false ;
        }
        if (held & (1 << BTN_7)) {
          if (millis() - btn7PressTime >= PATCH_HOLD_MS && !btn7HeldLong) {
            btn7HeldLong          = true ;
            usbMIDI.sendControlChange(92, audioPatchCursor + 1, 1) ; usbMIDI.send_now() ;
            // Write marker file to SD so slot shows as filled after reboot
            AudioNoInterrupts() ;
            char mname[24] ;
            snprintf(mname, 24, "/AUDIO%02d.TXT", audioPatchCursor + 1) ;
            if (!SD.exists(mname)) {
              File mf = SD.open(mname, FILE_WRITE) ;
              if (mf) mf.close() ;
            }
            AudioInterrupts() ;
            audioPatchFilled[audioPatchCursor] = true ;
            audioPatchFlashActive = true ;
            audioPatchFlashSaved  = true ;
            audioPatchFlashTimer  = millis() ;
            page2Updated[P2_AUDIO] = false ;
          }
        }
        if (released & (1 << BTN_7)) {
          if (!btn7HeldLong) {
            if (audioPatchFilled[audioPatchCursor]) {
              usbMIDI.sendControlChange(93, audioPatchCursor + 1, 1) ; usbMIDI.send_now() ;
              audioPatchFlashActive = true ;
              audioPatchFlashSaved  = false ;
              audioPatchFlashTimer  = millis() ;
              page2Updated[P2_AUDIO] = false ;
            }
          }
          btn7HeldLong = false ;
        }
      }

    } // end !enc8HeldLong

    // Spectrum runs outside enc8 guard (visual only, no side effects)
    if (page == 0) { drawSpectrum() ; }

    // ==================== CONTROL GROUP (pages 0+1) ====================
    if (page < 2) {
      for(int i = 0 ; i < 4 ; i++) {
        int temp_control = (4 * page) + i ;
        if (released & (1 << (BTN_1 + i))) {
          if (temp_control == 7) {
            memcpy(function_value, function_reset, sizeof(function_value)) ;
            for(int j = 0 ; j < 9 ; j++) { labelUpdated[j] = false ; graphUpdated[j] = false ; }
            for(int j = 0 ; j < 4 ; j++) { controlUpdated[j] = false ; }
            usbMIDI.sendControlChange(cc_control[7], 127, 1) ; usbMIDI.send_now() ;
            // Resend all encoder values so autowaaave latches at reset positions
            for(int j = 0 ; j < 18 ; j++) {
              if (cc_encoder[j] == 0) continue ;
              int sv = (j == 8) ? map(function_value[j], 0, 127, 0, 32) : function_value[j] ;
              usbMIDI.sendControlChange(cc_encoder[j], sv, 1) ;
              usbMIDI.send_now() ;
              delayMicroseconds(200) ;
            }
          } else {
            control[temp_control] = !control[temp_control] ;
            usbMIDI.sendControlChange(cc_control[temp_control], control[temp_control] ? 127 : 0, 1) ;
            usbMIDI.send_now() ;
          }
          controlUpdated[i] = false ;
        }
      }
    }

    // ==================== FUNCTION GROUP (pages 0+1) ====================
    if (page < 2) {
      for(int i = 0 ; i < 4 ; i++) {
        int temp_function = (4 * page) + i ;
        if (released & (1 << (BTN_5 + i))) {
          if (function[temp_function] > 0) { usbMIDI.sendControlChange(16 + (16 * function[temp_function]) + temp_function, 0, 1) ; usbMIDI.send_now() ; }
          function[temp_function]++ ;
          if (function[temp_function] == 4) { function[temp_function] = 0 ; }
          else { usbMIDI.sendControlChange(16 + (16 * function[temp_function]) + temp_function, 127, 1) ; usbMIDI.send_now() ; }
          functionUpdated[i] = false ;
          function_flash[i] = 1500 ;
          if (page == 0) { LED_ON[LED_1 + i] = min(1, function[temp_function]) ; }
        }
      }
      if (page == 0) {
        for(int i = 0 ; i < 4 ; i++) {
          function_flash[i] = max(0, function_flash[i] - (function[i] * (3 + (3 * !LED_ON[LED_1 + i])))) ;
          if (function_flash[i] == 0) {
            function_flash[i] = 1500 ;
            LED_ON[LED_1 + i] = (1 - LED_ON[LED_1 + i]) * min(1, function[i]) ;
          }
        }
      }
    }

    // ==================== ENCODERS 0-7 (pages 0+1) ====================
    if (page < 2) {
      int encLimit = (page == 0) ? 9 : 8 ;
      for(int i = 0 ; i < encLimit ; i++) {
        long pos = encoders[i]->read() ;
        int temp_function = i + (9 * page) ;
        if (pos == lastEncPos[temp_function]) continue ;
        if (temp_function >= 18) continue ;
        int dir = (pos > lastEncPos[temp_function]) ? -1 : 1 ;
        function_value[temp_function] += encoder_speed * dir * function_direction[temp_function] ;
        function_value[temp_function] = constrain(function_value[temp_function], 0, 127) ;
        lastEncPos[temp_function] = pos ;
        if (i < 8) { graphUpdated[i] = false ; }
        if (i == 8 && page == 0) { graphUpdated[8] = false ; }
        int sendVal = (temp_function == 8) ? map(function_value[temp_function], 0, 127, 0, 32) : function_value[temp_function] ;
        usbMIDI.sendControlChange(cc_encoder[temp_function], sendVal, 1) ; usbMIDI.send_now() ;
      }
    }

    // ==================== ENCODER 8 — page 1 clip menu ====================
    if (page == 1) {
      long pos8 = encoders[8]->read() ;
      enc8Accum += lastEncPos[17] - pos8 ;
      lastEncPos[17] = pos8 ;
      if (enc8Accum >= 4) {
        enc8Accum = 0 ;
        menuSelection = constrain(menuSelection + 1, 0, max(0, clipCount - 1)) ;
        if (menuSelection >= menuScroll + 3) menuScroll = menuSelection - 2 ;
        videoDisplayUpdated = false ;
      } else if (enc8Accum <= -4) {
        enc8Accum = 0 ;
        menuSelection = constrain(menuSelection - 1, 0, max(0, clipCount - 1)) ;
        if (menuSelection < menuScroll) menuScroll = menuSelection ;
        videoDisplayUpdated = false ;
      }
    }

    // ==================== PAGE 2 ENCODERS ====================
    if (page == 2) {
      // Encoder 0: luma key — threshold 2 (fast)
      {
        long pos0 = encoders[0]->read() ;
        enc0Accum += lastEncPos[0] - pos0 ;
        lastEncPos[0] = pos0 ;
        if (enc0Accum >= 2) {
          enc0Accum = 0 ;
          lumaKeyValue = constrain(lumaKeyValue + 1, 0, lumaHighValue) ;
          char cmdBuf[16] ; snprintf(cmdBuf, 16, "LUMA:%d", lumaLowEnabled ? lumaKeyValue : 0) ;
          sendVideoCommand(cmdBuf) ; page2Updated[P2_LUMA] = false ;
        } else if (enc0Accum <= -2) {
          enc0Accum = 0 ;
          lumaKeyValue = constrain(lumaKeyValue - 1, 0, lumaHighValue) ;
          char cmdBuf[16] ; snprintf(cmdBuf, 16, "LUMA:%d", lumaLowEnabled ? lumaKeyValue : 0) ;
          sendVideoCommand(cmdBuf) ; page2Updated[P2_LUMA] = false ;
        }
      }
      // Encoder 1: luma high — threshold 2, clamped above lumaKeyValue
      {
        long pos1 = encoders[1]->read() ;
        enc1Accum += lastEncPos[1] - pos1 ;
        lastEncPos[1] = pos1 ;
        if (enc1Accum >= 2) {
          enc1Accum = 0 ;
          lumaHighValue = constrain(lumaHighValue + 1, lumaKeyValue, 127) ;
          char cmdBuf[24] ; snprintf(cmdBuf, 24, "LUMA_HIGH:%d", lumaHighValue) ;
          sendVideoCommand(cmdBuf) ; page2Updated[P2_LUMA_HI] = false ;
        } else if (enc1Accum <= -2) {
          enc1Accum = 0 ;
          lumaHighValue = constrain(lumaHighValue - 1, lumaKeyValue, 127) ;
          char cmdBuf[24] ; snprintf(cmdBuf, 24, "LUMA_HIGH:%d", lumaHighValue) ;
          sendVideoCommand(cmdBuf) ; page2Updated[P2_LUMA_HI] = false ;
        }
      }

      // Encoder 2: fade to black — threshold 2
      {
        long pos2 = encoders[2]->read() ;
        enc2Accum += lastEncPos[2] - pos2 ;
        lastEncPos[2] = pos2 ;
        if (enc2Accum >= 2) {
          enc2Accum = 0 ;
          fadeToBlackValue = constrain(fadeToBlackValue + 2, 0, 127) ;
          usbMIDI.sendControlChange(2, fadeToBlackValue, 1) ; usbMIDI.send_now() ;
          page2Updated[P2_FADE] = false ;
        } else if (enc2Accum <= -2) {
          enc2Accum = 0 ;
          fadeToBlackValue = constrain(fadeToBlackValue - 2, 0, 127) ;
          usbMIDI.sendControlChange(2, fadeToBlackValue, 1) ; usbMIDI.send_now() ;
          page2Updated[P2_FADE] = false ;
        }
      }

      // Encoder 5: patch scroll — threshold 4
      {
        long pos5 = encoders[5]->read() ;
        enc5Accum += lastEncPos[5] - pos5 ;
        lastEncPos[5] = pos5 ;
        if (enc5Accum >= 4) {
          enc5Accum = 0 ;
          patchCursor = constrain(patchCursor + 1, 0, 31) ;
          if (patchCursor >= patchScroll + 3) patchScroll = patchCursor - 2 ;
          page2Updated[P2_PATCH] = false ;
        } else if (enc5Accum <= -4) {
          enc5Accum = 0 ;
          patchCursor = constrain(patchCursor - 1, 0, 31) ;
          if (patchCursor < patchScroll) patchScroll = patchCursor ;
          page2Updated[P2_PATCH] = false ;
        }
      }

      // Encoder 6: audio patch scroll — threshold 4
      {
        long pos6 = encoders[6]->read() ;
        enc6Accum += lastEncPos[6] - pos6 ;
        lastEncPos[6] = pos6 ;
        if (enc6Accum >= 4) {
          enc6Accum = 0 ;
          audioPatchCursor = constrain(audioPatchCursor + 1, 0, 31) ;
          if (audioPatchCursor >= audioPatchScroll + 3) audioPatchScroll = audioPatchCursor - 2 ;
          page2Updated[P2_AUDIO] = false ;
        } else if (enc6Accum <= -4) {
          enc6Accum = 0 ;
          audioPatchCursor = constrain(audioPatchCursor - 1, 0, 31) ;
          if (audioPatchCursor < audioPatchScroll) audioPatchScroll = audioPatchCursor ;
          page2Updated[P2_AUDIO] = false ;
        }
      }

      // Encoder 4: luma source — threshold 4
      {
        long pos4 = encoders[4]->read() ;
        enc4Accum += lastEncPos[4] - pos4 ;
        lastEncPos[4] = pos4 ;
        if (enc4Accum >= 4) {
          enc4Accum = 0 ;
          lumaSourceCursor = constrain(lumaSourceCursor + 1, 0, NUM_LUMA_SOURCES - 1) ;
          if (lumaSourceCursor >= lumaSourceScroll + 3) lumaSourceScroll = lumaSourceCursor - 2 ;
          page2Updated[P2_LUMA_SRC] = false ;
        } else if (enc4Accum <= -4) {
          enc4Accum = 0 ;
          lumaSourceCursor = constrain(lumaSourceCursor - 1, 0, NUM_LUMA_SOURCES - 1) ;
          if (lumaSourceCursor < lumaSourceScroll) lumaSourceScroll = lumaSourceCursor ;
          page2Updated[P2_LUMA_SRC] = false ;
        }
      }

      // Encoder 8: mix (threshold 2) or source list (threshold 4)
      {
        long pos8 = encoders[8]->read() ;
        enc8Accum += lastEncPos[17] - pos8 ;
        lastEncPos[17] = pos8 ;
        int threshold = (mixerSubMode == 1) ? 2 : 4 ;
        if (enc8Accum >= threshold) {
          enc8Accum = 0 ;
          if (mixerSubMode == 1) {
            mixValue = constrain(mixValue + 1, 0, 127) ;
            char cmdBuf[16] ; snprintf(cmdBuf, 16, "MIX:%d", mixValue) ;
            sendVideoCommand(cmdBuf) ; page2Updated[P2_MIXER] = false ;
          } else if (mixerSubMode == 0) {
            chASelection = constrain(chASelection + 1, 0, NUM_SOURCES - 1) ;
            if (chASelection >= chAScroll + 4) chAScroll = chASelection - 3 ;
            page2Updated[P2_MIXER] = false ;
          } else if (mixerSubMode == 2) {
            chBSelection = constrain(chBSelection + 1, 0, NUM_SOURCES - 1) ;
            if (chBSelection >= chBScroll + 4) chBScroll = chBSelection - 3 ;
            page2Updated[P2_MIXER] = false ;
          }
        } else if (enc8Accum <= -threshold) {
          enc8Accum = 0 ;
          if (mixerSubMode == 1) {
            mixValue = constrain(mixValue - 1, 0, 127) ;
            char cmdBuf[16] ; snprintf(cmdBuf, 16, "MIX:%d", mixValue) ;
            sendVideoCommand(cmdBuf) ; page2Updated[P2_MIXER] = false ;
          } else if (mixerSubMode == 0) {
            chASelection = constrain(chASelection - 1, 0, NUM_SOURCES - 1) ;
            if (chASelection < chAScroll) chAScroll = chASelection ;
            page2Updated[P2_MIXER] = false ;
          } else if (mixerSubMode == 2) {
            chBSelection = constrain(chBSelection - 1, 0, NUM_SOURCES - 1) ;
            if (chBSelection < chBScroll) chBScroll = chBSelection ;
            page2Updated[P2_MIXER] = false ;
          }
        }
      }
    }

    // ==================== SETTLE UP ====================
    lastButtons = stableButtons ;

    // LED BYTE — override ring LEDs only when ledOverride active (after PAGE_HOLD_MS)
    if (ledOverride) {
      int dispPage = (pendingPage >= 0) ? pendingPage : page ;
      // Flash current page, solid for pending if different
      bool flash0 = (page == 0 && pendingPage != 0) ? pageFlashState : (pendingPage == 0) ;
      bool flash1 = (page == 1 && pendingPage != 1) ? pageFlashState : (pendingPage == 1) ;
      bool flash2 = (page == 2 && pendingPage != 2) ? pageFlashState : (pendingPage == 2) ;
      // If no pending, just flash current
      if (pendingPage < 0) {
        flash0 = (page == 0) ? pageFlashState : false ;
        flash1 = (page == 1) ? pageFlashState : false ;
        flash2 = (page == 2) ? pageFlashState : false ;
      }
      LED_ON[LED_6] = flash0 ;
      LED_ON[LED_7] = flash1 ;
      LED_ON[LED_8] = flash2 ;
    }

    static byte lastLedByte = 0xFF ;
    for(int i = 0 ; i < 8 ; i++) { if (LED_ON[i]) ledByte |= (1 << i) ; }
    if (ledByte != lastLedByte) { setLEDs(ledByte) ; lastLedByte = ledByte ; }

    // Smooth mix value toward target
    if (page == 2) {
      float diff = (float)mixValue - mixValueSmoothed ;
      if (abs(diff) > 0.2f) {
        mixValueSmoothed += diff * MIX_SMOOTH ;
        page2Updated[P2_MIXER] = false ;
      } else {
        mixValueSmoothed = (float)mixValue ;
      }
    }

    if (patchFlashActive && millis() - patchFlashTimer > PATCH_FLASH_MS) {
      patchFlashActive = false ;
      page2Updated[P2_PATCH] = false ;
    }

    if (audioPatchFlashActive && millis() - audioPatchFlashTimer > PATCH_FLASH_MS) {
      audioPatchFlashActive = false ;
      page2Updated[P2_AUDIO] = false ;
    }

    executePatchLoad() ;

    // ==================== DRAW QUEUE ====================
    done = false ;

    if (!done && page == 2) {
      if (!done && !page2Updated[P2_FADE])    { drawFadeDisplay() ;       done = true ; return ; }
      if (!done && !page2Updated[P2_LUMA])    { drawLumaKeyDisplay() ;    done = true ; return ; }
      if (!done && !page2Updated[P2_LUMA_HI]){ drawLumaHighDisplay() ;   done = true ; return ; }
      if (!done && !page2Updated[P2_LUMA_SRC]) { drawLumaSourceDisplay() ; done = true ; return ; }
      if (!done && !page2Updated[P2_PATCH])   { drawPatchDisplay() ;      done = true ; return ; }
      if (!done && !page2Updated[P2_AUDIO]) { drawAudioPatchDisplay() ; done = true ; return ; }
      if (!done && !page2Updated[P2_MIXER] && !suppressDisplay8Draw) { drawMixerDisplay() ; done = true ; return ; }
      int blankScreens[] = {3, 7} ;
      for (int i = 0 ; i < 2 ; i++) {
        if (!page2ClearUpdated[i]) {
          clearScreen(blankScreens[i]) ;
          page2ClearUpdated[i] = true ;
          done = true ; return ;
        }
      }
    }

    if (!done && page == 1 && !videoDisplayUpdated && !suppressDisplay8Draw) { drawVideoDisplay() ; done = true ; return ; }
    if (!done && page == 0 && !graphUpdated[8] && !suppressDisplay8Draw)     { updateVolumeDisplay() ; graphUpdated[8] = true ; done = true ; return ; }

    if (!done) {
      for(int i = 0 ; i < 9 ; i++) {
        if (i == 8) { labelUpdated[8] = true ; continue ; }
        if (!labelUpdated[i]) {
          updateLabel(i, (page == 0) ? label[i] : label_B[i], 2) ;
          labelUpdated[i] = true ; done = true ; return ;
        }
      }
    }
    if (!done) {
      for(int i = 0 ; i < 4 ; i++) {
        if (!controlUpdated[i]) {
          int temp_control = (4 * page) + i ;
          updateControlLabel(i, control_label[temp_control], 1, control_type[temp_control], control[temp_control]) ;
          controlUpdated[i] = true ; done = true ; return ;
        }
      }
    }
    if (!done) {
      for(int i = 0 ; i < 4 ; i++) {
        if (!functionUpdated[i]) {
          int temp_function = (4 * page) + i ;
          updateFunctionLabel(i + 4, "null", 1, function_type[temp_function], temp_function) ;
          functionUpdated[i] = true ; done = true ; return ;
        }
      }
    }
    if (!done) {
      for(int i = 0 ; i < 8 ; i++) {
        if (!graphUpdated[i]) {
          int temp_function = i + (9 * page) ;
          updateGraph(i, function_value[temp_function], temp_function) ;
          graphUpdated[i] = true ; done = true ; return ;
        }
      }
    }
  }
}