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

// Saved base values when in reactive mode (restored on exit)
int  saved_function_value[18] { 0, 64, 63, 63, 63, 63, 63, 63, 100,   63, 63, 0, 0, 0, 63, 63, 0, 63 } ;

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

#define MAX_VIDEO_CLIPS 64
char clipNames[MAX_VIDEO_CLIPS][20] ;
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

char serialBuf[1024] ;
int  serialBufPos = 0 ;

// ============================================================
// MIXER STATE
// ============================================================
// The mixer on autoclip has two channels (A and B) that can be sourced
// from clips, composite inputs, or external USB capture. These settings
// control the video mixer via serial commands sent through video_bridge.

#define NUM_SOURCES 16
const char* sourceNames[NUM_SOURCES] = {
  "CLIPS", "COMPOSITE 1", "COMPOSITE 2", "COMPOSITE 3", "EXT USB", "SQUIRCLE", "MOIRE", "QUASI", "HYPNO", "CIRCUIT", "SMPTE", "ANCIENT", "SHATI", "ALIEN", "SLICES", "MANDELBRODT"
} ;

// Luma key source includes "NONE" as first option
#define NUM_LUMA_SOURCES 17
const char* lumaSourceNames[NUM_LUMA_SOURCES] = {
  "NONE", "CLIPS", "COMPOSITE 1", "COMPOSITE 2", "COMPOSITE 3", "EXT USB", "SQUIRCLE", "MOIRE", "QUASI", "HYPNO", "CIRCUIT", "SMPTE", "ANCIENT", "SHATI", "ALIEN", "SLICES", "MANDELBRODT"
} ;

// EXT USB availability (from autoclip, kept for potential status indicator)
bool extUsbAvailable = false ;

// EXT USB always shown in menus (displays black if device unplugged)
#define MAX_SOURCE_INDEX (NUM_SOURCES - 1)
#define MAX_LUMA_SOURCE_INDEX (NUM_LUMA_SOURCES - 1)

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
unsigned long lastMixerDisplayUpdate = 0 ;
#define MIXER_DISPLAY_INTERVAL_MS 50  // only redraw mixer display every 50ms (~20fps)

#define P2_LUMA      0
#define P2_LUMA_HI   1
#define P2_LUMA_SRC  2
#define P2_PATCH     3
#define P2_AUDIO     4
#define P2_MIXER     5
#define P2_FADE      6
#define P2_FFT_SMOOTH 7
#define P2_MOSH      8

int fftSmoothValue = 38 ;  // 0-127, default ~0.3 (38/127)
int moshValue = 0 ;    // 0-127, 0=off, 127=full mosh
int chromaValue = 51 ;  // 0-127, chromatic aberration amount (default 40%)
int crushValue = 0 ;   // 0-127, bit crush amount
int pixelValue = 13 ;   // 0-127, pixelize amount (default 10%)
int enc7Mode = 0 ;     // Selected post-FX row: 0=Mosh, 1=Chroma, 2=Crush, 3=Pixel
int postFxScroll = 0 ;
bool postFxEditing = false ;
#define ENC7_MODE_COUNT 4
const char* enc7ModeLabels[] = { "MOSH", "CHROMA", "CRUSH", "PIXEL" } ;
int fftBandDecayValue = 102 ; // 0-127, default ~0.8 (102/127) - additional decay when sustained
int fftHitValue = 96 ;        // 0-127, default 75% (96/127) - transient jump amount
int fftSlideValue = 64 ;      // 0-127, default 50% (64/127) - slide-in rate after transient
static long enc3Accum = 0 ;
int enc3Mode = 0 ;  // 0=Smooth, 1=Attack, 2=Decay, 3=B-Decay, 4=Hit, 5=Slide
#define ENC3_MODE_COUNT 6
const char* enc3ModeLabels[] = { "SMOOTH", "ATTACK", "DECAY", "B-DECAY", "HIT", "SLIDE" } ;
#define P2_COUNT     9
bool page2Updated[P2_COUNT] = { false } ;
bool spectrumPending = false ;
int  fadeToBlackValue    = 0 ;
static long enc2Accum    = 0 ;

// Settings screen state
int  settingsCursor      = 0 ;   // LUMA TRACK is first; the final row scrolls into view
int  settingsScroll      = 0 ;
bool lumaTrackEnabled    = true ;
bool fadeEnabled         = false ;
int  videoOutputMode     = 0 ;   // 0 = HDMI, 1 = COMP, 2 = BOTH
bool passthruEnabled     = false ;
bool houseCardEnabled    = false ;
bool houseLogoEnabled    = false ;
bool annLogoEnabled      = false ;
#define SETTINGS_COUNT   7

// Page switching
int  pendingPage     = -1 ;
bool enc8HeldLong    = false ;
bool pageSwitched    = false ;
bool ledOverride     = false ;  // true when showing page select LEDs
uint16_t ringConsumed = 0 ;     // ring buttons consumed for page select (ignore their release)

// Encoder accumulators
static long enc0Accum = 0 ;
static long enc4Accum = 0 ;
static long enc7Accum = 0 ;
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
AudioMixer4 passthruMixerL ;
AudioMixer4 passthruMixerR ;
AudioOutputI2S audioOut ;
AudioConnection patchCord1(audioIn, 0, mixer, 0) ;
AudioConnection patchCord2(audioIn, 1, mixer, 1) ;
AudioConnection patchCord3(mixer, 0, fft, 0) ;
AudioConnection patchCord4(audioIn, 0, passthruMixerL, 0) ;
AudioConnection patchCord5(audioIn, 1, passthruMixerR, 0) ;
AudioConnection patchCord6(passthruMixerL, 0, audioOut, 0) ;
AudioConnection patchCord7(passthruMixerR, 0, audioOut, 1) ;

float smoothedLevels[128] = { 0 } ;
float peakLevels[32] = { 0 } ;
unsigned long peakHoldStart[32] = { 0 } ;
#define PEAK_HOLD_MS 1000
#define PEAK_DECAY   0.0012f
#define ATTACK  0.8
#define DECAY 0.09

// =============================================================================
// CONTINUOUS FFT BAND OUTPUT
// =============================================================================
// Smoothed band values with asymmetric attack/decay for punchy response
float bandLow  = 0.0f ;
float bandMid  = 0.0f ;
float bandHigh = 0.0f ;

// Attack/decay rates (adjustable via encoder 3 on page 2)
float bandAttackRate = 0.5f ;  // 0-1, higher = faster attack
float bandDecayRate  = 0.8f ;  // 0-1, higher = faster decay
int fftAttackValue = 64 ;      // 0-127 for patch save
int fftDecayValue  = 102 ;     // 0-127 for patch save (102 = 0.8)

// CC numbers for continuous FFT output
#define CC_BAND_LOW  110
#define CC_BAND_MID  111
#define CC_BAND_HIGH 112

// Rate limiting: only send if value changed by more than threshold
int lastBandLowCC  = -1 ;
int lastBandMidCC  = -1 ;
int lastBandHighCC = -1 ;
#define BAND_CC_THRESHOLD 1  // minimum change to trigger send (reduces MIDI flood)
#define BAND_SEND_INTERVAL_MS 33  // minimum ms between sends (~30Hz max)
unsigned long lastBandSendTime = 0 ;
unsigned long fftCooldownUntil = 0 ;  // pause FFT MIDI after page changes
#define FFT_COOLDOWN_MS 2000  // 2s cooldown after page switch

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
    char buf[1024] ; strncpy(buf, msg + 5, 1023) ; buf[1023] = '\0' ;
    char* token = strtok(buf, ",") ;
    while (token != NULL && clipCount < MAX_VIDEO_CLIPS) {
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
  // EXT USB availability (hot-swap detection)
  else if (strncmp(msg, "EXT_USB:", 8) == 0) {
    extUsbAvailable = (atoi(msg + 8) == 1) ;
  }
  // Actual video-output state reported by autowaaave.
  else if (strncmp(msg, "VIDEO_OUT:", 10) == 0) {
    if (strncmp(msg + 10, "COMP", 4) == 0) videoOutputMode = 1 ;
    else if (strncmp(msg + 10, "BOTH", 4) == 0) videoOutputMode = 2 ;
    else videoOutputMode = 0 ;
    page2Updated[P2_FADE] = false ;
  }
  else if (strncmp(msg, "LUMA_TRACK:", 11) == 0) {
    lumaTrackEnabled = (strncmp(msg + 11, "ON", 2) == 0 || atoi(msg + 11) == 1) ;
    page2Updated[P2_FADE] = false ;
  }
  // Envelope params from Pi (audio patch load) - update display values
  // Format: ENV:smooth,attack,decay,bdecay[,hit,slide]
  else if (strncmp(msg, "ENV:", 4) == 0) {
    int s, a, d, b, h = 96, sl = 64 ;  // defaults for hit/slide if not provided
    int parsed = sscanf(msg + 4, "%d,%d,%d,%d,%d,%d", &s, &a, &d, &b, &h, &sl) ;
    if (parsed >= 4) {
      fftSmoothValue = constrain(s, 0, 127) ;
      fftAttackValue = constrain(a, 0, 127) ;
      fftDecayValue = constrain(d, 0, 127) ;
      fftBandDecayValue = constrain(b, 0, 127) ;
      bandAttackRate = fftAttackValue / 127.0f ;
      bandDecayRate = fftDecayValue / 127.0f ;
      if (parsed >= 6) {
        fftHitValue = constrain(h, 0, 127) ;
        fftSlideValue = constrain(sl, 0, 127) ;
      }
      page2Updated[P2_FFT_SMOOTH] = false ;  // Trigger display refresh
      Serial.println("ENV_OK") ;
    }
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
    } else if (serialBufPos < 639) { serialBuf[serialBufPos++] = c ; }
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

  // In reactive mode, show base value; otherwise show current value
  int baseValue = (react_band >= 0 && function_id < 8) ? saved_function_value[function_id] : value ;
  int reactValue = value ;

  if (function_direction[function_id] == -1) {
    baseValue = 127 - baseValue ;
    reactValue = 127 - reactValue ;
  }

  // Draw base bar
  if (function_graphType[function_id] == 0) {
    // Unipolar: bar from left
    displays[display_id].fillRect(1, 26, constrain(baseValue, 1, 126), 12, SSD1306_WHITE) ;
  } else {
    // Bipolar: bar from center
    int left  = constrain(map(baseValue, 1, 63, 1, 63), 1, 63) ;
    int right = constrain(map(baseValue, 64, 126, 63, 127), 63, 126) ;
    displays[display_id].fillRect(left, 26, right - left + 1, 12, SSD1306_WHITE) ;
  }

  // Draw reactivity feedback when in reactive mode
  if (react_band >= 0 && function_id < 8 && baseValue != reactValue) {
    // Calculate x positions - for unipolar, value maps directly to x position
    int xBase  = constrain(baseValue, 1, 126) ;
    int xReact = constrain(reactValue, 1, 126) ;

    if (xReact > xBase) {
      // Encoder moved right: show new region as WHITE (was black/empty)
      displays[display_id].fillRect(xBase + 1, 28, xReact - xBase, 8, SSD1306_WHITE) ;
      // XOR overlap at base position
      displays[display_id].fillRect(xBase, 28, 1, 8, SSD1306_BLACK) ;
    } else {
      // Encoder moved left: show removed region as BLACK (was white/filled)
      displays[display_id].fillRect(xReact, 28, xBase - xReact + 1, 8, SSD1306_BLACK) ;
    }

    // Draw white vertical line at react position
    displays[display_id].drawLine(xReact, 23, xReact, 40, SSD1306_WHITE) ;
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

// Match the clip selector: small centered title, instruction above a divider,
// and three 11-pixel rows at y=31, 42 and 53.
void drawMenuHeader(int display_id, const char* title, const char* hint) {
  displays[display_id].setTextSize(1) ;
  displays[display_id].setTextColor(SSD1306_WHITE) ;
  int16_t x1, y1 ; uint16_t w, h ;
  displays[display_id].getTextBounds(title, 0, 0, &x1, &y1, &w, &h) ;
  displays[display_id].setCursor((SCREEN_WIDTH - w) / 2, 1) ;
  displays[display_id].println(title) ;
  if (hint) {
    displays[display_id].setCursor(6, 19) ;
    displays[display_id].println(hint) ;
  }
  displays[display_id].drawFastHLine(2, 29, 124, SSD1306_WHITE) ;
}

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

void drawSettingsDisplay() {
  displays[2].clearDisplay() ;
  drawMenuHeader(2, "SETTINGS", "PUSH TO TOGGLE") ;

  const char* settingNames[] = { "LUMA TRACK", "FADE", "VIDEO OUTPUT", "AUDIO PASS", "HOUSE CARD", "HOUSE LOGO", "MAIN LOGO" } ;
  bool settingValues[] = { lumaTrackEnabled, fadeEnabled, false, passthruEnabled, houseCardEnabled, houseLogoEnabled, annLogoEnabled } ;

  for (int row = 0 ; row < 3 ; row++) {
    int i = settingsScroll + row ;
    if (i >= SETTINGS_COUNT) break ;
    int yPos = 31 + row * 11 ;
    if (i == settingsCursor) {
      displays[2].fillRect(2, yPos - 1, 124, 11, SSD1306_WHITE) ;
      displays[2].setTextColor(SSD1306_BLACK) ;
    } else {
      displays[2].setTextColor(SSD1306_WHITE) ;
    }
    displays[2].setCursor(6, yPos) ;
    displays[2].print(settingNames[i]) ;
    const char* stateText = (i == 2) ? (videoOutputMode == 0 ? "HDMI" : (videoOutputMode == 1 ? "COMP" : "BOTH")) : (settingValues[i] ? "ON" : "OFF") ;
    int16_t x1, y1 ; uint16_t w, h ;
    displays[2].getTextBounds(stateText, 0, 0, &x1, &y1, &w, &h) ;
    displays[2].setCursor(122 - w, yPos) ;
    displays[2].print(stateText) ;
  }
  displays[2].setTextColor(SSD1306_WHITE) ;
  displays[2].display() ;
  page2Updated[P2_FADE] = true ;
}

void drawFftSmoothDisplay() {
  displays[3].clearDisplay() ;
  displays[3].setTextColor(SSD1306_WHITE) ;
  int16_t x1, y1 ; uint16_t w, h ;

  // Get current mode label and value
  const char* label = enc3ModeLabels[enc3Mode] ;
  int currentValue ;
  switch (enc3Mode) {
    case 0: currentValue = fftSmoothValue ; break ;
    case 1: currentValue = fftAttackValue ; break ;
    case 2: currentValue = fftDecayValue ; break ;
    case 3: currentValue = fftBandDecayValue ; break ;
    case 4: currentValue = fftHitValue ; break ;
    case 5: currentValue = fftSlideValue ; break ;
    default: currentValue = fftSmoothValue ; break ;
  }

  // Draw label - use smaller font only for B-DECAY
  displays[3].setTextSize(2) ;
  displays[3].getTextBounds(label, 0, 0, &x1, &y1, &w, &h) ;
  displays[3].setCursor((128 - w) / 2, 0) ;
  displays[3].println(label) ;

  // Draw bar
  int barVal = constrain(currentValue, 0, 126) ;
  displays[3].fillRect(1, 26, barVal, 12, SSD1306_WHITE) ;
  displays[3].drawLine(1, 23, 1, 40, SSD1306_WHITE) ;
  displays[3].drawLine(126, 23, 126, 40, SSD1306_WHITE) ;
  displays[3].drawLine(1, 32, 126, 32, SSD1306_WHITE) ;

  // Show percentage
  char pctBuf[8] ;
  int pct = (currentValue * 100) / 127 ;
  snprintf(pctBuf, 8, "%d%%", pct) ;
  displays[3].getTextBounds(pctBuf, 0, 0, &x1, &y1, &w, &h) ;
  displays[3].setCursor((128 - w) / 2, 50) ;
  displays[3].println(pctBuf) ;
  displays[3].display() ;
  page2Updated[P2_FFT_SMOOTH] = true ;
}

void drawLumaSourceDisplay() {
  displays[4].clearDisplay() ;
  drawMenuHeader(4, "LUMA SOURCE", "PUSH TO SELECT") ;
  for (int i = 0 ; i < 3 ; i++) {
    int idx = lumaSourceScroll + i ;
    if (idx > MAX_LUMA_SOURCE_INDEX) break ;
    int yPos = 31 + i * 11 ;
    if (idx == lumaSourceCursor) {
      displays[4].fillRect(2, yPos - 1, 124, 11, SSD1306_WHITE) ;
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
    drawMenuHeader(8, header, "PUSH TO SELECT") ;
    for (int i = 0 ; i < 3 ; i++) {
      int idx = scrIdx + i ;
      if (idx > MAX_SOURCE_INDEX) break ;
      int yPos = 31 + i * 11 ;
      if (idx == selIdx) { displays[8].fillRect(2, yPos - 1, 124, 11, SSD1306_WHITE) ; displays[8].setTextColor(SSD1306_BLACK) ; }
      else { displays[8].setTextColor(SSD1306_WHITE) ; }
      if (idx == confirmed) { displays[8].setCursor(2, yPos) ; displays[8].print(">") ; displays[8].setCursor(14, yPos) ; }
      else { displays[8].setCursor(8, yPos) ; }
      displays[8].println(sourceNames[idx]) ;
    }
    displays[8].setTextColor(SSD1306_WHITE) ;
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
  drawMenuHeader(5, "VIDEO PATCHES", "HOLD SAVE / TAP LOAD") ;
  int16_t x1, y1 ; uint16_t w, h ;

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
    int yPos = 31 + i * 11 ;
    if (idx == patchCursor) {
      displays[5].fillRect(2, yPos - 1, 124, 11, SSD1306_WHITE) ;
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
  displays[5].display() ;
  page2Updated[P2_PATCH] = true ;
}

void drawAudioPatchDisplay() {
  displays[6].clearDisplay() ;
  drawMenuHeader(6, "AUDIO PATCHES", "HOLD SAVE / TAP LOAD") ;
  int16_t x1, y1 ; uint16_t w, h ;

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
    int yPos = 31 + i * 11 ;
    if (idx == audioPatchCursor) {
      displays[6].fillRect(2, yPos - 1, 124, 11, SSD1306_WHITE) ;
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
  displays[6].display() ;
  page2Updated[P2_AUDIO] = true ;
}

void drawMoshDisplay() {
  displays[7].clearDisplay() ;
  displays[7].setTextColor(SSD1306_WHITE) ;
  int16_t x1, y1 ; uint16_t w, h ;

  if (!postFxEditing) {
    // Match the clip selector's small font, 11-pixel rows and three-row scroll.
    drawMenuHeader(7, "POST FX", "PUSH TO EDIT") ;
    const int values[ENC7_MODE_COUNT] = { moshValue, chromaValue, crushValue, pixelValue } ;
    for (int i = 0 ; i < 3 ; i++) {
      int idx = postFxScroll + i ;
      if (idx >= ENC7_MODE_COUNT) break ;
      int yPos = 31 + i * 11 ;
      if (idx == enc7Mode) {
        displays[7].fillRect(2, yPos - 1, 124, 11, SSD1306_WHITE) ;
        displays[7].setTextColor(SSD1306_BLACK) ;
      } else {
        displays[7].setTextColor(SSD1306_WHITE) ;
      }
      displays[7].setCursor(6, yPos) ;
      displays[7].print(enc7ModeLabels[idx]) ;
      char valueBuf[16] ;
      snprintf(valueBuf, sizeof(valueBuf), "%d%%", values[idx] * 100 / 127) ;
      displays[7].getTextBounds(valueBuf, 0, 0, &x1, &y1, &w, &h) ;
      displays[7].setCursor(122 - w, yPos) ;
      displays[7].print(valueBuf) ;
    }
  } else {
    // Open the selected effect as a full-size value bar, like LUMA KEY.
    const int values[ENC7_MODE_COUNT] = { moshValue, chromaValue, crushValue, pixelValue } ;
    int currentValue = values[enc7Mode] ;
    const char* label = enc7ModeLabels[enc7Mode] ;
    displays[7].setTextSize(2) ;
    displays[7].getTextBounds(label, 0, 0, &x1, &y1, &w, &h) ;
    displays[7].setCursor((128 - w) / 2, 0) ;
    displays[7].println(label) ;
    int val = constrain(currentValue, 0, 126) ;
    displays[7].fillRect(1, 26, val, 12, SSD1306_WHITE) ;
    displays[7].drawLine(1, 23, 1, 40, SSD1306_WHITE) ;
    displays[7].drawLine(126, 23, 126, 40, SSD1306_WHITE) ;
    displays[7].drawLine(1, 32, 126, 32, SSD1306_WHITE) ;
    displays[7].drawFastHLine(2, 45, 124, SSD1306_WHITE) ;
    displays[7].setTextSize(1) ;
    char valueBuf[32] ;
    snprintf(valueBuf, sizeof(valueBuf), "%d%%  PUSH FOR LIST", currentValue * 100 / 127) ;
    displays[7].getTextBounds(valueBuf, 0, 0, &x1, &y1, &w, &h) ;
    displays[7].setCursor((128 - w) / 2, 49) ;
    displays[7].println(valueBuf) ;
  }
  displays[7].setTextColor(SSD1306_WHITE) ;
  displays[7].display() ;
  page2Updated[P2_MOSH] = true ;
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
      spectrumPending = true ;
    }
  }
}

// =============================================================================
// PROCESS FFT BANDS AND SEND MIDI
// =============================================================================
// Calculates low/mid/high band energies from FFT and sends as MIDI CCs.
// Uses asymmetric smoothing (fast attack, slow decay) for punchy response.
// Gain controlled by encoder 8 (function_value[8]) — same as spectrum display.

void processFftBands() {
  // Don't check fft.available() - just read current values
  // Rate limiting handles frequency, and this avoids conflict with drawSpectrum

  // Get input gain multiplier from encoder 8 (line-in volume)
  // Same scaling as drawSpectrum uses
  float gain = map(function_value[8], 0, 127, 0, 32) / 25.0f ;

  // If gain is zero, zero out bands and send zeros (once)
  if (gain < 0.01f) {
    if (lastBandLowCC != 0 || lastBandMidCC != 0 || lastBandHighCC != 0) {
      bandLow = bandMid = bandHigh = 0.0f ;
      Serial.println("FFT:0,0,0") ;
      lastBandLowCC = lastBandMidCC = lastBandHighCC = 0 ;
    }
    return ;
  }

  // Sum FFT bins into bands
  // FFT256 at 44.1kHz: each bin = 172Hz
  // We read bins (i*4)+1 like drawSpectrum does (every 4th bin, offset by 1)
  // Bin mapping (approximate):
  //   Bins 0-3:   0-688Hz    (bass/sub)
  //   Bins 4-11:  688-2064Hz (mids)
  //   Bins 12-31: 2064-5500Hz (highs/presence)

  float rawLow  = 0.0f ;
  float rawMid  = 0.0f ;
  float rawHigh = 0.0f ;

  // Low band: bins 0-3 (bass)
  for (int i = 0 ; i < 4 ; i++) {
    rawLow += fft.read((i * 4) + 1) ;
  }

  // Mid band: bins 4-11
  for (int i = 4 ; i < 12 ; i++) {
    rawMid += fft.read((i * 4) + 1) ;
  }

  // High band: bins 12-31
  for (int i = 12 ; i < 32 ; i++) {
    rawHigh += fft.read((i * 4) + 1) ;
  }

  // Apply gain and normalize (empirical scaling factors)
  // Lower values = more headroom before clipping
  rawLow  *= gain * 6.0f ;
  rawMid  *= gain * 4.0f ;
  rawHigh *= gain * 3.0f ;

  // Apply asymmetric smoothing (fast attack, slow decay)
  float rateLow  = (rawLow  > bandLow)  ? bandAttackRate : bandDecayRate ;
  float rateMid  = (rawMid  > bandMid)  ? bandAttackRate : bandDecayRate ;
  float rateHigh = (rawHigh > bandHigh) ? bandAttackRate : bandDecayRate ;

  bandLow  += (rawLow  - bandLow)  * rateLow ;
  bandMid  += (rawMid  - bandMid)  * rateMid ;
  bandHigh += (rawHigh - bandHigh) * rateHigh ;

  // Clamp raw values to 0-1 range, then convert to CC (0-127) for serial
  float clampedLow  = constrain(rawLow,  0.0f, 1.0f) ;
  float clampedMid  = constrain(rawMid,  0.0f, 1.0f) ;
  float clampedHigh = constrain(rawHigh, 0.0f, 1.0f) ;
  int ccLow  = (int)(clampedLow  * 127.0f) ;
  int ccMid  = (int)(clampedMid  * 127.0f) ;
  int ccHigh = (int)(clampedHigh * 127.0f) ;

  // Time-based rate limiting
  unsigned long now = millis() ;
  if (now - lastBandSendTime < BAND_SEND_INTERVAL_MS) return ;

  // Send over Serial instead of MIDI to avoid buffer issues
  bool changed = (abs(ccLow - lastBandLowCC) > BAND_CC_THRESHOLD) ||
                 (abs(ccMid - lastBandMidCC) > BAND_CC_THRESHOLD) ||
                 (abs(ccHigh - lastBandHighCC) > BAND_CC_THRESHOLD) ;

  if (changed) {
    char fftBuf[24] ;
    snprintf(fftBuf, 24, "FFT:%d,%d,%d", ccLow, ccMid, ccHigh) ;
    Serial.println(fftBuf) ;
    lastBandLowCC = ccLow ;
    lastBandMidCC = ccMid ;
    lastBandHighCC = ccHigh ;
    lastBandSendTime = now ;
  } else if (now - lastBandSendTime > 100) {
    // Heartbeat: force send every 100ms even if unchanged
    char fftBuf[24] ;
    snprintf(fftBuf, 24, "FFT:%d,%d,%d", ccLow, ccMid, ccHigh) ;
    Serial.println(fftBuf) ;
    lastBandSendTime = now ;
  }
}

// ============================================================
// MIDI
// ============================================================

void initializeAllMidiControls() {
  usbMIDI.sendControlChange(16, 64, 1) ; usbMIDI.send_now() ;
  for(byte cc = 17 ; cc <= 127 ; cc++) {
    // CC 94-97 are shader effects (mosh, chroma, crush, pixel)
    // Default: mosh=0, chroma=40% (51), crush=0, pixel=10% (13)
    byte val = 63 ;
    if (cc == 94 || cc == 96) val = 0 ;       // mosh, crush = 0
    else if (cc == 95) val = 51 ;              // chroma = 40%
    else if (cc == 97) val = 13 ;              // pixel = 10%
    usbMIDI.sendControlChange(cc, val, 1) ;
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
  f.print("lumaTrack=") ; f.println(lumaTrackEnabled ? 1 : 0) ;
  f.print("mixMode=")   ; f.println(mixerSubMode) ;
  f.print("clip=")      ; f.println(clipPos) ;
  f.print("loop=")      ; f.println(isLooping ? 1 : 0) ;
  f.print("srcHue=")    ; f.println(srcHue) ;
  f.print("srcSat=")    ; f.println(srcSat) ;
  f.print("srcVal=")    ; f.println(srcVal) ;
  f.print("srcShader=") ; f.println(srcShader) ;
  f.print("mosh=")      ; f.println(moshValue) ;
  f.print("chroma=")    ; f.println(chromaValue) ;
  f.print("crush=")     ; f.println(crushValue) ;
  f.print("pixel=")     ; f.println(pixelValue) ;
  f.print("houseCard=") ; f.println(houseCardEnabled ? 1 : 0) ;
  f.print("houseLogo=") ; f.println(houseLogoEnabled ? 1 : 0) ;
  f.print("mainLogo=")  ; f.println(annLogoEnabled ? 1 : 0) ;
  // Envelope values (fftSmooth, fftAttack, fftDecay, fftBdecay) are saved with audio patches, not video patches
  f.close() ;
  patchFilled[slot] = true ;
  snprintf(patchNames[slot], 10, "PATCH %02d", slot + 1) ;
  AudioInterrupts() ;
  char cueCmd[24] ;
  snprintf(cueCmd, 24, "CUES_SAVE:%d", slot) ;
  sendVideoCommand(cueCmd) ;
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
    else if (strcmp(key, "lumaTrack") == 0) { lumaTrackEnabled = (bool)val ; }
    else if (strcmp(key, "mixMode") == 0)   { mixerSubMode     = val ; }
    else if (strcmp(key, "clip") == 0)      { clipPos          = val ; }
    else if (strcmp(key, "loop") == 0)      { isLooping = (bool)val ; }
    else if (strcmp(key, "srcHue") == 0)    { srcHue           = val ; }
    else if (strcmp(key, "srcSat") == 0)    { srcSat           = val ; }
    else if (strcmp(key, "srcVal") == 0)    { srcVal           = val ; }
    else if (strcmp(key, "srcShader") == 0) { srcShader        = val ; }
    else if (strcmp(key, "mosh") == 0)      { moshValue        = val ; }
    else if (strcmp(key, "chroma") == 0)    { chromaValue      = val ; }
    else if (strcmp(key, "crush") == 0)     { crushValue       = val ; }
    else if (strcmp(key, "pixel") == 0)     { pixelValue       = val ; }
    else if (strcmp(key, "houseCard") == 0) { houseCardEnabled = (bool)val ; }
    else if (strcmp(key, "houseLogo") == 0) { houseLogoEnabled = (bool)val ; }
    else if (strcmp(key, "mainLogo") == 0)  { annLogoEnabled   = (bool)val ; }
    // Envelope values (fftSmooth, fftAttack, fftDecay, fftBdecay) are loaded with audio patches, not video patches
  }
  f.close() ;
  AudioInterrupts() ;
  char cmdBuf[32] ;
  snprintf(cmdBuf, 24, "LUMA:%d",         lumaLowEnabled ? lumaKeyValue : 0) ; queueVideoCommand(cmdBuf) ;
  snprintf(cmdBuf, 24, "LUMA_HIGH:%d",    lumaHighValue) ;                     queueVideoCommand(cmdBuf) ;
  snprintf(cmdBuf, 24, "LUMA_HIGH_EN:%d", lumaHighEnabled ? 1 : 0) ;           queueVideoCommand(cmdBuf) ;
  snprintf(cmdBuf, 24, "LUMA_SRC:%d",     lumaSourceSel) ;                     queueVideoCommand(cmdBuf) ;
  snprintf(cmdBuf, 24, "MIX:%d",          mixValue) ;                          queueVideoCommand(cmdBuf) ;
  snprintf(cmdBuf, 24, "CH_A:%d",         chASource) ;                         queueVideoCommand(cmdBuf) ;
  snprintf(cmdBuf, 24, "CH_B:%d",         chBSource) ;                         queueVideoCommand(cmdBuf) ;
  snprintf(cmdBuf, 24, "CUES_LOAD:%d",    slot) ;                              queueVideoCommand(cmdBuf) ;
  snprintf(cmdBuf, 24, "PLAYLOOP:%d:%d", clipPos, isLooping ? 1 : 0) ;          queueVideoCommand(cmdBuf) ;
  Serial.println(lumaTrackEnabled ? "LUMA_TRACK:ON" : "LUMA_TRACK:OFF") ;
  Serial.println(houseCardEnabled ? "HOUSECARD:1" : "HOUSECARD:0") ;
  Serial.println(houseLogoEnabled ? "HOUSELOGO:1" : "HOUSELOGO:0") ;
  Serial.println(annLogoEnabled ? "ANNLOGO:1" : "ANNLOGO:0") ;
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
    // Send datamosh value
    usbMIDI.sendControlChange(94, moshValue, 1) ; usbMIDI.send_now() ;
    patchLoadStep++ ; return ;
  }
  if (patchLoadStep == 37) {
    // Send chroma value
    usbMIDI.sendControlChange(95, chromaValue, 1) ; usbMIDI.send_now() ;
    patchLoadStep++ ; return ;
  }
  if (patchLoadStep == 38) {
    // Send crush value
    usbMIDI.sendControlChange(96, crushValue, 1) ; usbMIDI.send_now() ;
    patchLoadStep++ ; return ;
  }
  if (patchLoadStep == 39) {
    // Send pixel value
    usbMIDI.sendControlChange(97, pixelValue, 1) ; usbMIDI.send_now() ;
    patchLoadStep++ ; return ;
  }
  if (patchLoadStep == 40) {
    Serial.println("PATCH: CC91 LOW") ;
    usbMIDI.sendControlChange(91, 0, 1) ; usbMIDI.send_now() ;
    patchLoadStep++ ; return ;
  }
  if (patchLoadStep == 41) {
    patchLoadPending      = false ;
    patchFlashActive      = true ;
    patchFlashSaved       = false ;
    patchFlashTimer       = millis() ;
    page2Updated[P2_PATCH]     = false ;
    page2Updated[P2_LUMA]      = false ;
    page2Updated[P2_LUMA_HI]  = false ;
    page2Updated[P2_LUMA_SRC]   = false ;
    page2Updated[P2_MIXER]     = false ;
    page2Updated[P2_MOSH]      = false ;
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
  mixer.gain(0, 1.0) ;        // Left channel gain
  mixer.gain(1, 1.0) ;        // Right channel gain
  // Page 3 / screen 3 PASSTHRU controls these stereo monitor paths.
  // Start muted so the saved UI default and actual audio state agree.
  passthruMixerL.gain(0, 0.0) ;
  passthruMixerR.gain(0, 0.0) ;
  audioShield.volume(0.7) ;

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
  // Every page starts from a clean framebuffer. Normal-page widgets redraw
  // only their own regions, so a selective clear leaves menu pixels behind.
  // Flushes remain staggered by the draw queue to protect audio timing.
  for (int i = 0 ; i < 9 ; i++) { displays[i].clearDisplay() ; }
  spectrumPending = false ;
  page = newPage ;
  fftCooldownUntil = millis() + FFT_COOLDOWN_MS ;  // pause FFT MIDI during page transition
  invalidateAllDisplays() ;
  // Reset encoder accumulators and resync positions on page switch
  enc0Accum = 0 ;
  enc1Accum = 0 ;
  enc4Accum = 0 ;
  enc7Accum = 0 ;
  enc8Accum = 0 ;
  lastEncPos[0]  = encoders[0]->read() ;
  lastEncPos[1]  = encoders[1]->read() ;
  lastEncPos[4]  = encoders[4]->read() ;
  lastEncPos[7]  = encoders[7]->read() ;
  lastEncPos[8]  = encoders[8]->read() ;
  lastEncPos[17] = encoders[8]->read() ;
  // Resync scroll positions to current selections
  chAScroll = constrain(chASelection - 1, 0, max(0, MAX_SOURCE_INDEX - 2)) ;
  chBScroll = constrain(chBSelection - 1, 0, max(0, MAX_SOURCE_INDEX - 2)) ;
  lumaSourceScroll = constrain(lumaSourceCursor - 1, 0, max(0, MAX_LUMA_SOURCE_INDEX - 2)) ;
  settingsScroll = constrain(settingsCursor - 1, 0, max(0, SETTINGS_COUNT - 3)) ;
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
    postFxEditing = false ;
    postFxScroll = constrain(enc7Mode - 1, 0, max(0, ENC7_MODE_COUNT - 3)) ;
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
    bool previewDrewThisLoop = false ;

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
        ringConsumed |= (1 << BTN_LOW) ;  // mark consumed so release doesn't trigger page action
        if (prev != pendingPage) { previewPageDisplay8(pendingPage) ; suppressDisplay8Draw = true ; previewDrewThisLoop = true ; }
      }
      if (pressed & (1 << BTN_MID)) {
        int prev = pendingPage ; pendingPage = 1 ;
        ringConsumed |= (1 << BTN_MID) ;
        if (prev != pendingPage) { previewPageDisplay8(pendingPage) ; suppressDisplay8Draw = true ; previewDrewThisLoop = true ; }
      }
      if (pressed & (1 << BTN_HI)) {
        int prev = pendingPage ; pendingPage = 2 ;
        ringConsumed |= (1 << BTN_HI) ;
        if (prev != pendingPage) { previewPageDisplay8(pendingPage) ; suppressDisplay8Draw = true ; previewDrewThisLoop = true ; }
      }

      if (heldMs >= PAGE_HOLD_MS && !enc8HeldLong) {
        enc8HeldLong = true ;
        ledOverride  = true ;
        if (pendingPage < 0) { previewPageDisplay8(page) ; suppressDisplay8Draw = true ; previewDrewThisLoop = true ; }
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
      ringConsumed = 0 ;  // clear consumed flags
    }

    // ==================== PAGE-SPECIFIC CONTROLS ====================
    // Guard with !enc8HeldLong for short-press actions only
    // Ring button controls on each page are separate from page-select ring buttons

    if (!enc8HeldLong) {

      // PAGE 1 VIDEO CONTROLS
      if (page == 1) {
        if ((released & (1 << BTN_LOW)) && !(ringConsumed & (1 << BTN_LOW))) {
          isLooping = !isLooping ; LED_ON[LED_6] = isLooping ;
          sendVideoCommand(isLooping ? "LOOP_ON" : "LOOP_OFF") ;
          videoDisplayUpdated = false ;
        }
        if ((released & (1 << BTN_MID)) && !(ringConsumed & (1 << BTN_MID))) {
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
        if ((released & (1 << BTN_HI)) && !(ringConsumed & (1 << BTN_HI))) {
          sendVideoCommand("NEXT") ; LED_ON[LED_8] = true ; skipLedTime = millis() ;
        }
        if (LED_ON[LED_8] && millis() - skipLedTime > SKIP_LED_MS) { LED_ON[LED_8] = false ; }
      }

      // PAGE 0 REACTIVE — send on PRESS so encoder moves use correct mode
      if (page == 0) {
        for(int i = 0 ; i < 3 ; i++) {
          if ((pressed & (1 << (BTN_LOW + i))) && !(ringConsumed & (1 << (BTN_LOW + i)))) {
            int prev_band = react_band ;
            for(int ii = 0 ; ii < 3 ; ii++) { usbMIDI.sendControlChange(cc_reactive[ii], 0, 1) ; usbMIDI.send_now() ; }
            react_band = (react_band == i) ? -1 : i ;

            // Entering reactive mode: save base values
            if (prev_band < 0 && react_band >= 0) {
              for (int j = 0 ; j < 8 ; j++) { saved_function_value[j] = function_value[j] ; }
            }
            // Exiting reactive mode: restore base values and resend to autowaaave
            if (prev_band >= 0 && react_band < 0) {
              for (int j = 0 ; j < 8 ; j++) {
                function_value[j] = saved_function_value[j] ;
                graphUpdated[j] = false ;
                int sv = (j == 8) ? map(function_value[j], 0, 127, 0, 32) : function_value[j] ;
                usbMIDI.sendControlChange(cc_encoder[j], sv, 1) ; usbMIDI.send_now() ;
              }
            }

            if (react_band >= 0) { usbMIDI.sendControlChange(cc_reactive[react_band], 127, 1) ; usbMIDI.send_now() ; }
          }
        }
        LED_ON[LED_6] = (react_band == 0) ;
        LED_ON[LED_7] = (react_band == 1) ;
        LED_ON[LED_8] = (react_band == 2) ;
      }

      // PAGE 2 MIXER RING BUTTONS + CONTROLS
      if (page == 2) {
        if ((released & (1 << BTN_LOW)) && !(ringConsumed & (1 << BTN_LOW))) {
          mixerSubMode = 0 ;
          LED_ON[LED_6] = true ; LED_ON[LED_7] = false ; LED_ON[LED_8] = false ;
          page2Updated[P2_MIXER] = false ;
        }
        if ((released & (1 << BTN_MID)) && !(ringConsumed & (1 << BTN_MID))) {
          mixerSubMode = 1 ;
          LED_ON[LED_6] = false ; LED_ON[LED_7] = true ; LED_ON[LED_8] = false ;
          page2Updated[P2_MIXER] = false ;
        }
        if ((released & (1 << BTN_HI)) && !(ringConsumed & (1 << BTN_HI))) {
          mixerSubMode = 2 ;
          LED_ON[LED_6] = false ; LED_ON[LED_7] = false ; LED_ON[LED_8] = true ;
          page2Updated[P2_MIXER] = false ;
        }
        // BTN_3 = enc2 push — toggle selected setting
        if (released & (1 << BTN_3)) {
          if (settingsCursor == 0) {
            // Toggle adaptive scene-brightness compensation on autowaaave.
            lumaTrackEnabled = !lumaTrackEnabled ;
            Serial.println(lumaTrackEnabled ? "LUMA_TRACK:ON" : "LUMA_TRACK:OFF") ;
          } else if (settingsCursor == 1) {
            // Toggle FADE
            fadeEnabled = !fadeEnabled ;
            fadeToBlackValue = fadeEnabled ? 127 : 0 ;
            usbMIDI.sendControlChange(2, fadeToBlackValue, 1) ; usbMIDI.send_now() ;
          } else if (settingsCursor == 2) {
            // Cycle external output; autowaaave replies with the applied mode.
            videoOutputMode = (videoOutputMode + 1) % 3 ;
            if (videoOutputMode == 1) Serial.println("VIDEO_OUT:COMP") ;
            else if (videoOutputMode == 2) Serial.println("VIDEO_OUT:BOTH") ;
            else Serial.println("VIDEO_OUT:HDMI") ;
          } else if (settingsCursor == 3) {
            // Toggle stereo line-in monitoring through the Audio Shield output.
            passthruEnabled = !passthruEnabled ;
            float passthruGain = passthruEnabled ? 1.0f : 0.0f ;
            passthruMixerL.gain(0, passthruGain) ;
            passthruMixerR.gain(0, passthruGain) ;
          } else if (settingsCursor == 4) {
            // Toggle HOUSE CARD - mutual exclusivity with other overlays
            houseCardEnabled = !houseCardEnabled ;
            if (houseCardEnabled) { houseLogoEnabled = false ; annLogoEnabled = false ; }
            Serial.println(houseCardEnabled ? "HOUSECARD:1" : "HOUSECARD:0") ;
            Serial.println("HOUSELOGO:0") ; Serial.println("ANNLOGO:0") ;
          } else if (settingsCursor == 5) {
            // Toggle HOUSE LOGO - mutual exclusivity with other overlays
            houseLogoEnabled = !houseLogoEnabled ;
            if (houseLogoEnabled) { houseCardEnabled = false ; annLogoEnabled = false ; }
            Serial.println(houseLogoEnabled ? "HOUSELOGO:1" : "HOUSELOGO:0") ;
            Serial.println("HOUSECARD:0") ; Serial.println("ANNLOGO:0") ;
          } else if (settingsCursor == 6) {
            // Toggle ANN LOGO - mutual exclusivity with other overlays
            annLogoEnabled = !annLogoEnabled ;
            if (annLogoEnabled) { houseCardEnabled = false ; houseLogoEnabled = false ; }
            Serial.println(annLogoEnabled ? "ANNLOGO:1" : "ANNLOGO:0") ;
            Serial.println("HOUSECARD:0") ; Serial.println("HOUSELOGO:0") ;
          }
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
        // BTN_4 = cycle FFT param mode (Smooth/Attack/Decay/B-Decay)
        if (released & (1 << BTN_4)) {
          enc3Mode = (enc3Mode + 1) % ENC3_MODE_COUNT ;
          page2Updated[P2_FFT_SMOOTH] = false ;
        }
        // BTN_5 = confirm luma source selection
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
              page2Updated[P2_FFT_SMOOTH] = false ;  // Refresh envelope display
            }
          }
          btn7HeldLong = false ;
        }
        // BTN_8 = enter the selected post-FX graph, or return to its list.
        if (released & (1 << BTN_8)) {
          postFxEditing = !postFxEditing ;
          enc7Accum = 0 ;
          lastEncPos[7] = encoders[7]->read() ;
          page2Updated[P2_MOSH] = false ;
        }
      }

    } // end !enc8HeldLong

    // Process FFT bands FIRST (before any blocking display ops)
    processFftBands() ;

    // Spectrum runs outside enc8 guard (visual only, no side effects)
    if (page == 0) { drawSpectrum() ; }

    usbMIDI.read() ;  // keep USB healthy

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

      // Encoder 2: scroll through settings — threshold 4
      {
        long pos2 = encoders[2]->read() ;
        enc2Accum += lastEncPos[2] - pos2 ;
        lastEncPos[2] = pos2 ;
        if (enc2Accum >= 4) {
          enc2Accum = 0 ;
          settingsCursor = (settingsCursor + 1) % SETTINGS_COUNT ;
          if (settingsCursor >= settingsScroll + 3) settingsScroll = settingsCursor - 2 ;
          if (settingsCursor < settingsScroll) settingsScroll = settingsCursor ;
          page2Updated[P2_FADE] = false ;
        } else if (enc2Accum <= -4) {
          enc2Accum = 0 ;
          settingsCursor = (settingsCursor + SETTINGS_COUNT - 1) % SETTINGS_COUNT ;
          if (settingsCursor >= settingsScroll + 3) settingsScroll = settingsCursor - 2 ;
          if (settingsCursor < settingsScroll) settingsScroll = settingsCursor ;
          page2Updated[P2_FADE] = false ;
        }
      }

      // Encoder 3: FFT params (Smooth/Attack/Decay/B-Decay) — threshold 2
      {
        long pos3 = encoders[3]->read() ;
        enc3Accum += lastEncPos[3] - pos3 ;
        lastEncPos[3] = pos3 ;
        int delta = 0 ;
        if (enc3Accum >= 2) {
          enc3Accum = 0 ;
          delta = 2 ;
        } else if (enc3Accum <= -2) {
          enc3Accum = 0 ;
          delta = -2 ;
        }
        if (delta != 0) {
          char cmdBuf[32] ;
          switch (enc3Mode) {
            case 0:  // Smooth
              fftSmoothValue = constrain(fftSmoothValue + delta, 0, 127) ;
              snprintf(cmdBuf, 32, "FFT_SMOOTH:%d", fftSmoothValue) ;
              break ;
            case 1:  // Attack
              fftAttackValue = constrain(fftAttackValue + delta, 0, 127) ;
              bandAttackRate = fftAttackValue / 127.0f ;
              snprintf(cmdBuf, 32, "FFT_ATTACK:%d", fftAttackValue) ;
              break ;
            case 2:  // Decay
              fftDecayValue = constrain(fftDecayValue + delta, 0, 127) ;
              bandDecayRate = fftDecayValue / 127.0f ;
              snprintf(cmdBuf, 32, "FFT_DECAY:%d", fftDecayValue) ;
              break ;
            case 3:  // B-Decay (band decay)
              fftBandDecayValue = constrain(fftBandDecayValue + delta, 0, 127) ;
              snprintf(cmdBuf, 32, "FFT_BDECAY:%d", fftBandDecayValue) ;
              break ;
            case 4:  // Hit (transient jump amount)
              fftHitValue = constrain(fftHitValue + delta, 0, 127) ;
              snprintf(cmdBuf, 32, "FFT_HIT:%d", fftHitValue) ;
              break ;
            case 5:  // Slide (slide-in rate after transient)
              fftSlideValue = constrain(fftSlideValue + delta, 0, 127) ;
              snprintf(cmdBuf, 32, "FFT_SLIDE:%d", fftSlideValue) ;
              break ;
          }
          Serial.println(cmdBuf) ;
          page2Updated[P2_FFT_SMOOTH] = false ;
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

      // Encoder 7: scroll the post-FX list; edit only after pressing it.
      {
        long pos7 = encoders[7]->read() ;
        enc7Accum += lastEncPos[7] - pos7 ;
        lastEncPos[7] = pos7 ;
        int delta = 0 ;
        int threshold = postFxEditing ? 2 : 4 ;
        if (enc7Accum >= threshold) {
          enc7Accum = 0 ;
          delta = 1 ;
        } else if (enc7Accum <= -threshold) {
          enc7Accum = 0 ;
          delta = -1 ;
        }
        if (delta != 0) {
          if (!postFxEditing) {
            enc7Mode = constrain(enc7Mode + delta, 0, ENC7_MODE_COUNT - 1) ;
            if (enc7Mode >= postFxScroll + 3) postFxScroll = enc7Mode - 2 ;
            if (enc7Mode < postFxScroll) postFxScroll = enc7Mode ;
          } else {
            switch (enc7Mode) {
              case 0:  // Mosh
                moshValue = constrain(moshValue + delta, 0, 127) ;
                usbMIDI.sendControlChange(94, moshValue, 1) ; usbMIDI.send_now() ;
                break ;
              case 1:  // Chroma
                chromaValue = constrain(chromaValue + delta, 0, 127) ;
                usbMIDI.sendControlChange(95, chromaValue, 1) ; usbMIDI.send_now() ;
                break ;
              case 2:  // Crush
                crushValue = constrain(crushValue + delta, 0, 127) ;
                usbMIDI.sendControlChange(96, crushValue, 1) ; usbMIDI.send_now() ;
                break ;
              case 3:  // Pixel
                pixelValue = constrain(pixelValue + delta, 0, 127) ;
                usbMIDI.sendControlChange(97, pixelValue, 1) ; usbMIDI.send_now() ;
                break ;
            }
          }
          page2Updated[P2_MOSH] = false ;
        }
      }

      // Encoder 4: luma source — threshold 4
      {
        long pos4 = encoders[4]->read() ;
        enc4Accum += lastEncPos[4] - pos4 ;
        lastEncPos[4] = pos4 ;
        if (enc4Accum >= 4) {
          enc4Accum = 0 ;
          lumaSourceCursor = constrain(lumaSourceCursor + 1, 0, MAX_LUMA_SOURCE_INDEX) ;
          if (lumaSourceCursor >= lumaSourceScroll + 3) lumaSourceScroll = lumaSourceCursor - 2 ;
          page2Updated[P2_LUMA_SRC] = false ;
        } else if (enc4Accum <= -4) {
          enc4Accum = 0 ;
          lumaSourceCursor = constrain(lumaSourceCursor - 1, 0, MAX_LUMA_SOURCE_INDEX) ;
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
            chASelection = constrain(chASelection + 1, 0, MAX_SOURCE_INDEX) ;
            if (chASelection >= chAScroll + 3) chAScroll = chASelection - 2 ;
            page2Updated[P2_MIXER] = false ;
          } else if (mixerSubMode == 2) {
            chBSelection = constrain(chBSelection + 1, 0, MAX_SOURCE_INDEX) ;
            if (chBSelection >= chBScroll + 3) chBScroll = chBSelection - 2 ;
            page2Updated[P2_MIXER] = false ;
          }
        } else if (enc8Accum <= -threshold) {
          enc8Accum = 0 ;
          if (mixerSubMode == 1) {
            mixValue = constrain(mixValue - 1, 0, 127) ;
            char cmdBuf[16] ; snprintf(cmdBuf, 16, "MIX:%d", mixValue) ;
            sendVideoCommand(cmdBuf) ; page2Updated[P2_MIXER] = false ;
          } else if (mixerSubMode == 0) {
            chASelection = constrain(chASelection - 1, 0, MAX_SOURCE_INDEX) ;
            if (chASelection < chAScroll) chAScroll = chASelection ;
            page2Updated[P2_MIXER] = false ;
          } else if (mixerSubMode == 2) {
            chBSelection = constrain(chBSelection - 1, 0, MAX_SOURCE_INDEX) ;
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

    // Smooth mix value toward target (rate-limited display updates)
    if (page == 2) {
      float diff = (float)mixValue - mixValueSmoothed ;
      if (abs(diff) > 0.2f) {
        mixValueSmoothed += diff * MIX_SMOOTH ;
        // Only invalidate display at limited rate to avoid starving MIDI
        if (millis() - lastMixerDisplayUpdate > MIXER_DISPLAY_INTERVAL_MS) {
          page2Updated[P2_MIXER] = false ;
          lastMixerDisplayUpdate = millis() ;
        }
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
    // A page preview already transferred OLED 9 during this loop.
    if (previewDrewThisLoop) return ;
    usbMIDI.read() ;  // keep USB healthy before display draws
    done = false ;

    if (!done && page == 2) {
      if (!done && !page2Updated[P2_FADE])    { drawSettingsDisplay() ;   done = true ; return ; }
      if (!done && !page2Updated[P2_FFT_SMOOTH]) { drawFftSmoothDisplay() ; done = true ; return ; }
      if (!done && !page2Updated[P2_LUMA])    { drawLumaKeyDisplay() ;    done = true ; return ; }
      if (!done && !page2Updated[P2_LUMA_HI]){ drawLumaHighDisplay() ;   done = true ; return ; }
      if (!done && !page2Updated[P2_LUMA_SRC]) { drawLumaSourceDisplay() ; done = true ; return ; }
      if (!done && !page2Updated[P2_PATCH])   { drawPatchDisplay() ;      done = true ; return ; }
      if (!done && !page2Updated[P2_AUDIO]) { drawAudioPatchDisplay() ; done = true ; return ; }
      if (!done && !page2Updated[P2_MOSH])  { drawMoshDisplay() ;       done = true ; return ; }
      if (!done && !page2Updated[P2_MIXER] && !suppressDisplay8Draw) { drawMixerDisplay() ; done = true ; return ; }
      // All page 2 displays now in use (no blank screens)
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
    if (page == 0 && spectrumPending && !enc8HeldLong && pendingPage < 0) {
      displays[8].display() ;
      spectrumPending = false ;
    }
  }
}
