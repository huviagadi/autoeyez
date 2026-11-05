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
 *     Encoder 0: Source Config menu and low/high threshold editors
 *     Encoder 1: Renderer menu (luma-key inversion and mirror)
 *     Encoder 2: Existing global settings menu
 *     Encoder 4: Luma key source select
 *     Encoder 5: Video patch browser
 *     Encoder 6: Audio patch browser
 *     Encoder 8: A/B crossfader or channel source select
 *     Ring buttons: Select Ch A / Mixer view / Ch B
 *
 * PATCH SYSTEM:
 *   - 64 video patches stored on SD card (PATCH01.TXT through PATCH64.TXT)
 *   - 64 audio patches stored in autowaaave (triggered via MIDI CC 92/93)
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
#include "safe_patch_store.h"
#include "video_patch_schema.h"
#include "clip_list_receiver.h"
#include "clip_folder_menu.h"
#include "patch_journal.h"
#include "recall_queue.h"
#include "patch_slot_bounds.h"
#include "midi_parameter_defaults.h"
#include "fft_bands.h"
#include "fft_spectrum.h"
#include "line_in_gain.h"
#include "source_config_menu.h"
#include "beat_clock.h"
static_assert(AUDIO_SAMPLE_RATE_EXACT == 44100.0f, "Recalibrate FFT bands for a different sample rate");

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
// Physical layout: LEDs 1-4 pulse the current beat, 5-8 retain their controls.
#define LED_1 3
#define LED_2 4
#define LED_3 5
#define LED_4 6
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
autoeyez::VideoPatch pendingVideoPatch;
char patchTransactionId[17]={};
char patchTransactionOperation[6]={};
int patchTransactionSlot=-1;
bool patchSaveHadPrevious=false;
bool sourceStateSynced=false;
autoeyez::RecallQueue queuedRecalls;
unsigned long recallQueueFullAt=0;
bool patchTransactionUncertain=false,patchRecoveringSave=false,patchRecoverUndo=false;
autoeyez::PendingSave pendingSave;
bool recordPendingSave(const void* data,size_t length);
bool restorePendingSave();
void recoverPendingSave();
bool erasePatchJournal();
unsigned long patchTransactionSent=0;
void finishPatchTransaction(const char* status);
void beginPatchTransaction(const char* operation,int slot,const autoeyez::VideoPatch* patch);
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
int  react_band = -1 ;
bool control[8] = { false, false, false, false, false, false, false, false } ;
bool control_type[8] = { 0, 0, 0, 0, 0, 0, 0, 1 } ;

int cc_reactive[3] = { 43, 44, 42 } ;
autoeyez::SourceConfigMenu sourceConfigMenu;
int rendererCursor=0;
bool rendererLumaInvert=false, rendererMirror=false;

int cc_control[8] = { 60, 46, 62, 61, 41, 45, 71, 59 } ;

bool labelUpdated[9]     = { false, false, false, false, false, false, false, false, false } ;
bool graphUpdated[9]     = { false, false, false, false, false, false, false, false, false } ;
bool controlUpdated[4]   = { false, false, false, false } ;
bool functionUpdated[4]  = { false, false, false, false } ;

#define MAX_VIDEO_CLIPS 64
char clipNames[MAX_VIDEO_CLIPS][20] ;
int  clipCount      = 0 ;
autoeyez::ClipFolderMenu clipMenu;
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

autoeyez::SerialLineBuffer serialLines;
autoeyez::ClipListReceiver clipListReceiver;

// ============================================================
// MIXER STATE
// ============================================================
// The mixer on autoclip has two channels (A and B) that can be sourced
// from clips, composite inputs, or external USB capture. These settings
// control the video mixer via serial commands sent through video_bridge.

#define NUM_SOURCES 28
const char* sourceNames[NUM_SOURCES] = {
  "CLIPS", "COMPOSITE 1", "COMPOSITE 2", "COMPOSITE 3", "EXT USB", "SQUIRCLE", "MOIRE", "QUASI", "HYPNO", "CIRCUIT", "SMPTE", "ANCIENT", "SHATI", "ALIEN", "SLICES", "MANDELBRODT", "ASHANOHA", "TILES", "CURLESQUE", "SEIGAIHA", "TRI-PATTERN", "EYE OF EZBHAN", "CHROMAWAVE", "ICEBREAKER", "VOXELSCAPE", "POLY MANGLE", "RADIAL BANDS", "SLOW SCOPE"
} ;

// Luma key source includes "NONE" as first option
#define NUM_LUMA_SOURCES 29
const char* lumaSourceNames[NUM_LUMA_SOURCES] = {
  "NONE", "CLIPS", "COMPOSITE 1", "COMPOSITE 2", "COMPOSITE 3", "EXT USB", "SQUIRCLE", "MOIRE", "QUASI", "HYPNO", "CIRCUIT", "SMPTE", "ANCIENT", "SHATI", "ALIEN", "SLICES", "MANDELBRODT", "ASHANOHA", "TILES", "CURLESQUE", "SEIGAIHA", "TRI-PATTERN", "EYE OF EZBHAN", "CHROMAWAVE", "ICEBREAKER", "VOXELSCAPE", "POLY MANGLE", "RADIAL BANDS", "SLOW SCOPE"
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
bool     patchFilled[autoeyez::videoPatchSlotCount]      = { false } ;
char     patchNames[autoeyez::videoPatchSlotCount][10] ;
unsigned long btn6PressTime   = 0 ;
bool     btn6HeldLong         = false ;
bool     patchFlashActive     = false ;
unsigned long patchFlashTimer = 0 ;
bool     patchFlashSaved      = false ;
bool     patchFlashError      = false ;
static long enc6Accum           = 0 ;
int      audioPatchCursor       = 0 ;
int      audioPatchScroll       = 0 ;
bool     audioPatchFilled[autoeyez::audioPatchSlotCount]   = { false } ;
char     audioPatchNames[autoeyez::audioPatchSlotCount][10] ;
unsigned long btn7PressTime     = 0 ;
bool     btn7HeldLong           = false ;
bool     audioPatchFlashActive  = false ;
unsigned long audioPatchFlashTimer = 0 ;
bool     audioPatchFlashSaved   = false ;
bool     audioPatchFlashError = false ;
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
int codecLevel = 0, codecStatic = 0, codecLow = 100, codecMid = 80, codecHigh = 80, codecRate = 64 ;
void sendCodecSettings() {
  Serial.print("CODEC:"); Serial.print(codecLevel); Serial.print(',');
  Serial.print(codecLow); Serial.print(','); Serial.print(codecMid); Serial.print(',');
  Serial.print(codecHigh); Serial.print(','); Serial.print(codecRate); Serial.print(',');
  Serial.println(codecStatic);
}
int chromaValue = 51 ;  // 0-127, chromatic aberration amount (default 40%)
int crushValue = 0 ;   // 0-127, bit crush amount
int pixelValue = 13 ;   // 0-127, pixelize amount (default 10%)
int postFxReactiveValues[3][4] = {} ; // Mosh, Chroma, Crush, Pixel band depths
int bleedValue = 0, alignValue = 0, ghostValue = 0, wobbleValue = 0, overdriveValue = 0 ;
int holdValue = 0, holdReactiveValues[3] = {} ;
int peekValue = 0, peekReactiveValues[3] = {127, 127, 127}, peekMaskModeValue = 0 ;
int processingReactiveValues[3][5] = {} ;
int enc7Mode = 0 ;     // Selected processing row
int postFxScroll = 0 ;
bool postFxEditing = false ;
#define ENC7_MODE_COUNT 17
const char* enc7ModeLabels[] = { "AUDIO CODEC", "STATIC CODEC", "CHROMA", "CRUSH", "PIXEL", "BLEED", "ALIGN", "GHOST", "WOBBLE", "OVERDRIVE", "HOLD", "PEEK", "PEEK MASK", "CD LOW", "CD MID", "CD HIGH", "CD RATE" } ;
int fftBandDecayValue = 102 ; // 0-127, default ~0.8 (102/127) - additional decay when sustained
int fftHitValue = 96 ;        // 0-127, default 75% (96/127) - transient jump amount
int fftSlideValue = 64 ;      // 0-127, default 50% (64/127) - slide-in rate after transient
static long enc3Accum = 0 ;
int enc3Mode = 0 ;  // 0=Smooth, 1=Attack, 2=Decay, 3=B-Decay, 4=Hit, 5=Slide
#define ENC3_MODE_COUNT 6
const char* enc3ModeLabels[] = { "SMOOTH", "ATTACK", "DECAY", "B-DECAY", "HIT", "SLIDE" } ;
#define P2_COUNT     9
bool page2Updated[P2_COUNT] = { false } ;
// Short-lived shader feedback replaces only OLED 9 and yields to any local
// mainboard input. Source/parameter values arrive from the macropad via Pi2.
bool shaderOverlayActive = false ;
bool shaderOverlayDirty = false ;
unsigned long shaderOverlaySince = 0 ;
int shaderOverlayRole = 0, shaderOverlayBand = -1 ;
int shaderOverlaySource = -1, shaderOverlaySlot = -1, shaderOverlayValue = 0 ;
int shaderOverlayBase = 64, shaderOverlayPreview = 64 ;
char shaderOverlayLabel[20] = "SOURCE" ;
long shaderOverlayEncoderPos[9] = {} ;
constexpr unsigned long SHADER_OVERLAY_MS = 2500 ;
int videoFps = 0 ;
bool videoFpsValid = false, videoFpsDirty = false ;
unsigned long videoFpsAt = 0 ;

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
static long lineInGainAccum = 0 ;

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
  "INVERT BRIGHT", "TOROIDAL", "INVERT HUE", "INVERT SAT",
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

void dismissShaderOverlay() {
  if (!shaderOverlayActive) return ;
  shaderOverlayActive = shaderOverlayDirty = false ;
  displays[8].clearDisplay() ;
  if (page == 0) graphUpdated[8] = false ;
  else if (page == 1) videoDisplayUpdated = false ;
  else page2Updated[P2_MIXER] = false ;
}

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

autoeyez::FftSpectrum spectrum;

// =============================================================================
// CONTINUOUS FFT BAND OUTPUT
// =============================================================================
// Smoothed band values with asymmetric attack/decay for punchy response
autoeyez::FftBands fftBandProfile = autoeyez::FftBands::Contiguous;
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
#define CC_BAND_FRAME 113
#define CC_BAND_EXT_FIRST 114  // three 14-bit values, MSB then LSB
#define BAND_EXT_MAX 32.0f
#define FFT_SERIAL_COMPAT 0

// Rate limiting: only send if value changed by more than threshold
int lastBandLowCC  = -1 ;
int lastBandMidCC  = -1 ;
int lastBandHighCC = -1 ;
int lastBandExt[3] = {-1, -1, -1} ;
#define BAND_CC_THRESHOLD 0  // preserve one-step changes at low signal levels
#define BAND_SEND_INTERVAL_MS 10  // trial: carry each ~11.6ms averaged FFT snapshot
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
#include "controller_perf.h"
#include "controller_work.h"
autoeyez::ControllerPerf controllerPerf;
autoeyez::OledBudget oledBudget;
autoeyez::FftSnapshot fftSnapshot;
autoeyez::BeatClock beatClock;
uint32_t spectrumGeneration=0;
void transferControllerDisplay(int index){unsigned long started=micros();displays[index].display();controllerPerf.display(micros()-started);}
void drawVideoFpsBadge(){
  auto& screen = displays[8] ;
  screen.fillRect(107, 0, 21, 10, SSD1306_BLACK) ;
  if (!videoFpsValid) return ;
  char value[4] ; snprintf(value, sizeof(value), "%d", videoFps) ;
  screen.setTextSize(1) ; screen.setTextColor(SSD1306_WHITE) ;
  screen.setCursor(126 - int(strlen(value)) * 6, 1) ;
  screen.print(value) ;
}
void flushDisplay(int index){
  if(index==8)drawVideoFpsBadge();
  if(!midiInitialized)transferControllerDisplay(index);else oledBudget.mark(index);
}
void serviceControllerOled(){if(!midiInitialized)return;int index=oledBudget.next(micros());if(index>=0)transferControllerDisplay(index);}
void refreshControllerFft(){
  AudioNoInterrupts();
  if(fft.available()){fftSnapshot.capture(fft.output);controllerPerf.fftReads+=128;}
  AudioInterrupts();
}
float readControllerFft(unsigned bin){return fftSnapshot.read(bin);}
autoeyez::BandEnergy controllerBands(float gain){
  static bool valid=false;static uint32_t generation=0;static float previousGain=0;
  static autoeyez::FftBands profile=autoeyez::FftBands::Contiguous;static autoeyez::BandEnergy bands{0,0,0};
  if(!valid||generation!=fftSnapshot.generation||gain!=previousGain||profile!=fftBandProfile){
    bands=autoeyez::readFftBands([](unsigned int bin){return readControllerFft(bin);},gain,fftBandProfile);
    valid=true;generation=fftSnapshot.generation;previousGain=gain;profile=fftBandProfile;
  }
  return bands;
}
void reportControllerPerf(){
  if(!booted || !midiInitialized || millis()<10000)return;
  static unsigned long reported=0;if(millis()-reported<5000)return;reported=millis();
  char message[360];snprintf(message,sizeof(message),"CTRL_PERF:{\"page\":%d,\"loops\":%lu,\"loop_p95_us\":%lu,\"loop_max_us\":%lu,\"gap_max_us\":%lu,\"oled_count\":%lu,\"oled_total_us\":%lu,\"oled_max_us\":%lu,\"oled_per_loop_max\":%lu,\"fft_reads\":%lu,\"beat_src\":%u,\"beat_step\":%u,\"beat_ms\":%lu,\"beat_candidates\":%u}",page,(unsigned long)controllerPerf.loops,(unsigned long)controllerPerf.p95(),(unsigned long)controllerPerf.workMax,(unsigned long)controllerPerf.gapMax,(unsigned long)controllerPerf.oledCount,(unsigned long)controllerPerf.oledTotal,(unsigned long)controllerPerf.oledMax,(unsigned long)controllerPerf.oledPerLoopMax,(unsigned long)controllerPerf.fftReads,unsigned(beatClock.source),unsigned(beatClock.step),(unsigned long)beatClock.periodMs,unsigned(beatClock.candidateCount));
  if(Serial.availableForWrite()>=int(strlen(message)+2)){
    Serial.println(message);controllerPerf.reset();
    if (beatClock.source==autoeyez::BeatClock::Audio && beatClock.candidateCount>1) {
      char onsetLine[240];int used=snprintf(onsetLine,sizeof(onsetLine),"BEAT_CAND:%lu",(unsigned long)beatClock.periodMs);
      unsigned count=min(unsigned(beatClock.candidateCount-1),unsigned(16));
      for(unsigned i=0;i<count && used<int(sizeof(onsetLine))-16;++i)
        used+=snprintf(onsetLine+used,sizeof(onsetLine)-used,",%lu:%u",(unsigned long)(beatClock.candidates[i]-beatClock.candidates[i+1]),unsigned(beatClock.candidateStrength[i]*10));
      if(Serial.availableForWrite()>=used+2)Serial.println(onsetLine);
    }
  }
}
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
  // Patch transactions use their own acknowledged path; controls keep flowing.
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
void restorePatchBackup(int slot) ;
void loadPatch(int slot) ;
void handleSerialMessage(const char* msg) {
  if (strncmp(msg, "CODEC_STATE:", 12) == 0) {
    int audio, low, mid, high, rate, staticAmount; char extra;
    if (sscanf(msg + 12, "%d,%d,%d,%d,%d,%d%c", &audio, &low, &mid,
               &high, &rate, &staticAmount, &extra) == 6 &&
        audio >= 0 && audio <= 127 && low >= 0 && low <= 127 &&
        mid >= 0 && mid <= 127 && high >= 0 && high <= 127 &&
        rate >= 0 && rate <= 127 && staticAmount >= 0 && staticAmount <= 127) {
      codecLevel = audio; codecLow = low; codecMid = mid;
      codecHigh = high; codecRate = rate; codecStatic = staticAmount;
      sendCodecSettings();
      page2Updated[P2_MOSH] = false;
    }
    return;
  }
  if (strncmp(msg, "PATCH_LOAD:", 11) == 0) {
    const char* number = msg + 11 ;
    if (number[0] >= '0' && number[0] <= '9' &&
        number[1] >= '0' && number[1] <= '9' && number[2] == '\0') {
      const int slot = (number[0] - '0') * 10 + number[1] - '0' - 1 ;
      if (autoeyez::validControllerVideoSlot(slot)) loadPatch(slot) ;
      else Serial.println("PATCH_LOAD_ERROR:INVALID_SLOT") ;
    } else Serial.println("PATCH_LOAD_ERROR:INVALID_SLOT") ;
    return ;
  }
  if (strncmp(msg, "PATCH_BACKUP_RESTORE:", 21) == 0) {
    const char* number = msg + 21 ;
    if (number[0] >= '0' && number[0] <= '9' &&
        number[1] >= '0' && number[1] <= '9' && number[2] == '\0') {
      restorePatchBackup((number[0] - '0') * 10 + number[1] - '0' - 1) ;
    } else Serial.println("PATCH_BACKUP_ERROR:INVALID_SLOT") ;
    return ;
  }
  if (strncmp(msg, "FPS:", 4) == 0) {
    const char* value = msg + 4 ;
    if (*value && strlen(value) <= 3) {
      int fps = 0 ; bool valid = true ;
      for (const char* digit = value ; *digit ; ++digit) {
        if (*digit < '0' || *digit > '9') { valid = false ; break ; }
        fps = fps * 10 + (*digit - '0') ;
      }
      if (valid) {
        videoFpsDirty = !videoFpsValid || fps != videoFps ;
        videoFps = fps ; videoFpsValid = true ; videoFpsAt = millis() ;
      }
    }
    return ;
  }
  if (strncmp(msg, "SHADER_UI:", 10) == 0) {
    int role, band, source, slot, value, base, preview ;
    char label[20] = {} ;
    if (sscanf(msg + 10, "%d,%d,%d,%d,%d,%d,%d,%19[^\n]",
               &role, &band, &source, &slot, &value, &base, &preview, label) == 8 &&
        role >= 0 && role < 3 && band >= -1 && band < 3 &&
        source >= -1 && source < NUM_SOURCES && slot >= -1 && slot < 3 &&
        value >= 0 && value <= 127 && base >= 0 && base <= 127 &&
        preview >= 0 && preview <= 127 && label[0] != '\0') {
      shaderOverlayRole = role ; shaderOverlayBand = band ;
      shaderOverlaySource = source ; shaderOverlaySlot = slot ;
      shaderOverlayValue = value ; shaderOverlayBase = base ;
      shaderOverlayPreview = preview ;
      strncpy(shaderOverlayLabel, label, sizeof(shaderOverlayLabel)-1) ;
      shaderOverlayLabel[sizeof(shaderOverlayLabel)-1] = '\0' ;
      for (int i = 0 ; i < 9 ; ++i) shaderOverlayEncoderPos[i] = encoders[i]->read() ;
      shaderOverlaySince = millis() ;
      shaderOverlayActive = shaderOverlayDirty = true ;
    }
    return ;
  }
  if (strncmp(msg,"BEAT:",5)==0) {
    if (msg[5]>='1' && msg[5]<='4' && msg[6]=='\0')
      beatClock.midiBeat(uint8_t(msg[5]-'0'),millis());
    return;
  }
  if (strcmp(msg,"BEAT_STOP")==0) { beatClock.midiStop(); return; }
  char listAck[64];
  if(clipListReceiver.handle(msg,millis(),clipNames,clipCount,listAck,sizeof(listAck))){
    if(clipListReceiver.published){
      clipMenu.reset(clipListReceiver.completedCrc);clipMenu.clamp(clipCount);videoDisplayUpdated=false;
    }
    if(listAck[0]&&Serial.availableForWrite()>=int(strlen(listAck)+2))Serial.println(listAck);
    return;
  }
  if(strncmp(msg,"FOL1:",5)==0){
    if(clipListReceiver.completed[0] && clipMenu.receive(msg,clipListReceiver.completedCrc,clipCount))videoDisplayUpdated=false;
    return;
  }
  if (strncmp(msg,"PTX_ACK:",8)==0) {
    if(patchLoadPending && strncmp(msg+8,patchTransactionId,16)==0 && msg[24]==':')finishPatchTransaction(msg+25);
    return;
  }
  if(strncmp(msg,"SOURCE_STATE:",13)==0){
    int a,b,mix,low,high,cut,source,enabled;char extra;
    if(sscanf(msg+13,"%d,%d,%d,%d,%d,%d,%d,%d%c",&a,&b,&mix,&low,&high,&cut,&source,&enabled,&extra)==8 && a>=0&&a<NUM_SOURCES&&b>=0&&b<NUM_SOURCES&&mix>=0&&mix<=127&&low>=0&&low<=high&&high<=127&&(cut==0||cut==1)&&source>=0&&source<NUM_LUMA_SOURCES&&(enabled==0||enabled==1)){
      chASource=chASelection=a;chBSource=chBSelection=b;mixValue=mix;mixValueSmoothed=mix;
      if(enabled)lumaKeyValue=low;lumaHighValue=high;lumaLowEnabled=enabled;lumaHighEnabled=cut;lumaSourceSel=lumaSourceCursor=source;
      sourceStateSynced=true;page2Updated[P2_LUMA]=false;page2Updated[P2_LUMA_HI]=false;page2Updated[P2_MIXER]=false;page2Updated[P2_LUMA_SRC]=false;
    }
    return;
  }
  // System ready signal from video_bridge (autowaaave is running)
  if (strncmp(msg, "START", 5) == 0) {
    autowaaaveReady = true ;
    if(CrashReport){Serial.println("CONTROLLER_CRASH_REPORT");Serial.print(CrashReport);}
    Serial.println("GOT_START") ;
    Serial.println(passthruEnabled ? "AUDIO_PASS:ON" : "AUDIO_PASS:OFF") ;
    Serial.print("SHADER_BAND:");Serial.println(react_band);
    sendCodecSettings();
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
    // A legacy replay cannot overwrite a checksum-confirmed modern list.
    if(clipListReceiver.completed[0])return;
    char staged[MAX_VIDEO_CLIPS][20]={};int count=0;const char*cursor=msg+5;
    while(*cursor){
      if(count==MAX_VIDEO_CLIPS)return;
      const char*comma=strchr(cursor,',');size_t length=comma?size_t(comma-cursor):strlen(cursor);
      if(length==0)return;
      memcpy(staged[count],cursor,min(length,size_t(19)));count++;
      if(!comma)break;cursor=comma+1;if(!*cursor)return;
    }
    memcpy(clipNames,staged,sizeof(staged));clipCount=count;
    clipMenu.reset(0);clipMenu.clamp(clipCount);
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
  else if (strcmp(msg, "BANDS?") == 0) {
    Serial.print("BANDS_OK:"); Serial.println(autoeyez::bandProfileName(fftBandProfile));
  }
  else if (strncmp(msg, "BANDS:", 6) == 0) {
    autoeyez::FftBands requested;
    if (autoeyez::parseBandProfile(msg + 6, requested)) {
      if (requested != fftBandProfile) {
        fftBandProfile = requested;
        bandLow = bandMid = bandHigh = 0.0f;
        lastBandLowCC = lastBandMidCC = lastBandHighCC = -999;
        lastBandSendTime = 0;
      }
      Serial.print("BANDS_OK:"); Serial.println(autoeyez::bandProfileName(fftBandProfile));
    } else Serial.println("BANDS_ERROR");
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
  while(Serial.available()){
    serialLines.feed(char(Serial.read()),[](const char* line,unsigned length){
      if(length==1&&line[0]=='R')_reboot_Teensyduino_();
      else handleSerialMessage(line);
    });
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
  flushDisplay(display_id) ;
}

void updateLabel(int display_id, const char* text, int textSize) {
  displays[display_id].fillRect(0, 0, SCREEN_WIDTH, 20, SSD1306_BLACK) ;
  displays[display_id].setTextColor(SSD1306_WHITE) ;
  displays[display_id].setTextSize(textSize) ;
  int16_t x1, y1 ; uint16_t w, h ;
  displays[display_id].getTextBounds(text, 0, 0, &x1, &y1, &w, &h) ;
  displays[display_id].setCursor((SCREEN_WIDTH - w) / 2, 0) ;
  displays[display_id].println(text) ;
  flushDisplay(display_id) ;
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
  flushDisplay(display_id) ;
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
  flushDisplay(display_id) ;
}

void updateGraph(int display_id, int value, int function_id) {
  displays[display_id].fillRect(0, 19, SCREEN_WIDTH, 22, SSD1306_BLACK) ;

  // Both encoder pages show the held pre-reactive value and live offset.
  bool reactiveGraph = react_band >= 0 &&
                       (function_id < 8 || (function_id >= 9 && function_id < 17)) ;
  int baseValue = reactiveGraph ? saved_function_value[function_id] : value ;
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
  if (reactiveGraph && baseValue != reactValue) {
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
  flushDisplay(display_id) ;
}

// ============================================================
// PAGE 2 DISPLAY FUNCTIONS
// ============================================================

// Page 3 lists share a title, divider, and four 12-pixel row slots.
constexpr int MENU_VISIBLE_ROWS = 4 ;
constexpr int MENU_ROW_TOP = 15 ;
constexpr int MENU_ROW_HEIGHT = 12 ;
int menuRowY(int row) { return MENU_ROW_TOP + row * MENU_ROW_HEIGHT ; }

void keepMenuCursorVisible(int cursor, int& scroll, int count) {
  scroll = constrain(scroll, 0, max(0, count - MENU_VISIBLE_ROWS)) ;
  if (cursor < scroll) scroll = cursor ;
  if (cursor >= scroll + MENU_VISIBLE_ROWS) scroll = cursor - MENU_VISIBLE_ROWS + 1 ;
}

void drawMenuHeader(int display_id, const char* title) {
  displays[display_id].setTextSize(1) ;
  displays[display_id].setTextColor(SSD1306_WHITE) ;
  int16_t x1, y1 ; uint16_t w, h ;
  displays[display_id].getTextBounds(title, 0, 0, &x1, &y1, &w, &h) ;
  displays[display_id].setCursor((SCREEN_WIDTH - w) / 2, 1) ;
  displays[display_id].println(title) ;
  displays[display_id].drawFastHLine(2, 11, 124, SSD1306_WHITE) ;
}

void drawLumaKeyDisplay() {
  auto& d = displays[0]; d.clearDisplay(); d.setTextColor(SSD1306_WHITE);
  if (sourceConfigMenu.editing) {
    d.setTextSize(2);
    const char* title = sourceConfigMenu.cursor == 1 ? "LUMA LOW" : "LUMA HI";
    int16_t x,y; uint16_t w,h; d.getTextBounds(title,0,0,&x,&y,&w,&h);
    d.setCursor((128-w)/2,0); d.print(title);
    int value = sourceConfigMenu.cursor == 1 ? lumaKeyValue : lumaHighValue;
    d.fillRect(1,26,constrain(value,0,126),12,SSD1306_WHITE);
    d.drawLine(1,23,1,40,SSD1306_WHITE); d.drawLine(126,23,126,40,SSD1306_WHITE);
    d.drawLine(63,23,63,40,SSD1306_WHITE); d.drawLine(1,32,126,32,SSD1306_WHITE);
    d.setTextSize(1); d.setCursor(25,50); d.print("CLICK: BACK");
  } else {
    drawMenuHeader(0, "SOURCE CONFIG") ;
    const char* labels[] = {"LUMA SOURCE", "LUMA LOW", "LUMA CUT", "LUMA HI"} ;
    for (int row = 0 ; row < MENU_VISIBLE_ROWS ; row++) {
      int i = row ;
      int y = menuRowY(row) ;
      if (i == sourceConfigMenu.cursor) {
        d.fillRect(2, y - 1, 124, 11, SSD1306_WHITE) ;
        d.setTextColor(SSD1306_BLACK) ;
      } else d.setTextColor(SSD1306_WHITE) ;
      d.setCursor(6, y) ; d.print(labels[i]) ;
      char valueBuf[16] ;
      if (i == 0) snprintf(valueBuf, sizeof(valueBuf), "%s", lumaLowEnabled ? "ON" : "OFF") ;
      else if (i == 2) snprintf(valueBuf, sizeof(valueBuf), "%s", lumaHighEnabled ? "ON" : "OFF") ;
      else snprintf(valueBuf, sizeof(valueBuf), "%d", i == 1 ? lumaKeyValue : lumaHighValue) ;
      int16_t x1, y1 ; uint16_t w, h ;
      d.getTextBounds(valueBuf, 0, 0, &x1, &y1, &w, &h) ;
      d.setCursor(122 - w, y) ; d.print(valueBuf) ;
    }
  }
  flushDisplay(0); page2Updated[P2_LUMA]=true;
}

void drawLumaHighDisplay() {
  auto& d=displays[1];d.clearDisplay();d.setTextSize(1);d.setTextColor(SSD1306_WHITE);
  drawMenuHeader(1, "RENDERER") ;
  const char* labels[]={"LUMA KEY INV","MIRROR"};
  bool states[]={rendererLumaInvert,rendererMirror};
  for(int i=0;i<2;++i){int y=menuRowY(i);
    if(i==rendererCursor){d.fillRect(2,y-1,124,11,SSD1306_WHITE);d.setTextColor(SSD1306_BLACK);}
    else d.setTextColor(SSD1306_WHITE);
    d.setCursor(6,y);d.print(labels[i]);
    d.setCursor(states[i]?110:104,y);d.print(states[i]?"ON":"OFF");
  }
  d.setTextColor(SSD1306_WHITE) ;
  flushDisplay(1);page2Updated[P2_LUMA_HI]=true;
}

void drawSettingsDisplay() {
  displays[2].clearDisplay() ;
  drawMenuHeader(2, "SETTINGS") ;

  // Setting names
  const char* settingNames[] = { "LUMA TRACK", "FADE", "VIDEO OUTPUT", "AUDIO PASS", "HOUSE CARD", "HOUSE LOGO", "MAIN LOGO" } ;
  bool settingValues[] = { lumaTrackEnabled, fadeEnabled, false, passthruEnabled, houseCardEnabled, houseLogoEnabled, annLogoEnabled } ;

  for (int row = 0 ; row < MENU_VISIBLE_ROWS ; row++) {
    int i = settingsScroll + row ;
    if (i >= SETTINGS_COUNT) break ;
    int yPos = menuRowY(row) ;
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
  flushDisplay(2) ;
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
  flushDisplay(3) ;
  page2Updated[P2_FFT_SMOOTH] = true ;
}

void drawLumaSourceDisplay() {
  displays[4].clearDisplay() ;
  drawMenuHeader(4, "LUMA SOURCE") ;
  for (int i = 0 ; i < MENU_VISIBLE_ROWS ; i++) {
    int idx = lumaSourceScroll + i ;
    if (idx > MAX_LUMA_SOURCE_INDEX) break ;
    int yPos = menuRowY(i) ;
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
  flushDisplay(4) ;
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
    drawMenuHeader(8, header) ;
    for (int i = 0 ; i < MENU_VISIBLE_ROWS ; i++) {
      int idx = scrIdx + i ;
      if (idx > MAX_SOURCE_INDEX) break ;
      int yPos = menuRowY(i) ;
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
  flushDisplay(8) ;
  page2Updated[P2_MIXER] = true ;
}

void drawShaderOverlay() {
  auto& screen = displays[8] ;
  screen.clearDisplay() ;
  screen.setTextColor(SSD1306_WHITE) ;
  screen.setTextSize(1) ;
  const char* role = shaderOverlayRole == 0 ? "A" :
                     shaderOverlayRole == 1 ? "LUMA" : "B" ;
  const char* band = shaderOverlayBand < 0 ? "OFFSET" :
                     shaderOverlayBand == 0 ? "LOW" :
                     shaderOverlayBand == 1 ? "MID" : "HIGH" ;
  screen.setCursor(2, 1) ; screen.print(role) ; screen.print(' ') ; screen.print(band) ;
  screen.drawFastHLine(1, 11, 126, SSD1306_WHITE) ;
  const char* source = shaderOverlaySource >= 0 ?
                       sourceNames[shaderOverlaySource] : "NONE" ;
  if (shaderOverlaySlot < 0) {
    screen.setCursor(2, 19) ; screen.print("SELECTED SOURCE") ;
    int16_t x1, y1 ; uint16_t w, h ;
    screen.getTextBounds(source, 0, 0, &x1, &y1, &w, &h) ;
    screen.setCursor(max(2, (128 - int(w))/2), 37) ;
    screen.print(source) ;
  } else {
    screen.setCursor(2, 15) ; screen.print(source) ;
    screen.setCursor(2, 27) ; screen.print(shaderOverlayLabel) ;
    screen.drawRect(2, 41, 124, 11, SSD1306_WHITE) ;
    const int bar = map(shaderOverlayBase, 0, 127, 0, 120) ;
    if (bar > 0) screen.fillRect(4, 43, bar, 7, SSD1306_WHITE) ;
    if (shaderOverlayBand >= 0) {
      const int baseX = 4 + bar ;
      const int previewX = 4 + map(shaderOverlayPreview, 0, 127, 0, 120) ;
      if (previewX > baseX) screen.fillRect(baseX + 1, 44, previewX - baseX, 5, SSD1306_WHITE) ;
      if (previewX < baseX) screen.fillRect(previewX, 44, baseX - previewX + 1, 5, SSD1306_BLACK) ;
      screen.drawFastVLine(previewX, 38, 16, SSD1306_WHITE) ;
    }
    screen.setCursor(2, 55) ;
    screen.print("KNOB ") ; screen.print(shaderOverlaySlot + 1) ;
    screen.setCursor(89, 55) ;
    if (shaderOverlayBand >= 0) {
      const int depth = shaderOverlayValue - 64 ;
      if (depth > 0) screen.print('+') ;
      screen.print(depth) ;
    } else screen.print(shaderOverlayValue) ;
  }
  flushDisplay(8) ;
  shaderOverlayDirty = false ;
}

void drawClipMenuRows() {
  auto& screen=displays[8];
  for(int i=0;i<3;i++){
    int row=clipMenu.scroll+i;if(row>=clipMenu.rows(clipCount))break;
    int y=31+i*11;
    if(row==clipMenu.selection){screen.fillRect(2,y-1,124,11,SSD1306_WHITE);screen.setTextColor(SSD1306_BLACK);}
    else screen.setTextColor(SSD1306_WHITE);
    int clip=clipMenu.clipAtRow(row,clipCount);
    if(clip>=0&&clip==clipPos){screen.setCursor(2,y);screen.print(">");screen.setCursor(11,y);}
    else screen.setCursor(6,y);
    if(clipMenu.active<0){screen.print(clipMenu.names[clipMenu.folderAtRootRow(row)]);screen.println("/");}
    else if(row==0)screen.println("< BACK");
    else if(clip>=0)screen.println(clipNames[clip]);
  }
  screen.setTextColor(SSD1306_WHITE);
}

void activateClipMenuSelection(){
  int clip=clipMenu.select(clipCount);
  if(clip>=0){
    if(clip==clipPos){isPlaying=!isPlaying;LED_ON[LED_7]=isPlaying;sendVideoCommand(isPlaying?"PLAY":"PAUSE");}
    else{isPlaying=true;LED_ON[LED_7]=true;char command[16];snprintf(command,sizeof(command),"PLAY:%d",clip);sendVideoCommand(command);}
  }
  videoDisplayUpdated=false;
}

void drawLineInHeading() {
  displays[8].setTextSize(1);
  displays[8].setTextColor(SSD1306_WHITE);
  displays[8].setCursor(2, 1);
  displays[8].print("LINE IN");
  // The live FPS badge clears x=107..127 on every display flush.
  displays[8].setCursor(76, 1);
  displays[8].print(autoeyez::lineInGain(function_value[8]), 1);
  displays[8].print('x');
}

// Draw display 8 as it looks on a given page — for seamless page select preview
void previewPageDisplay8(int p) {
  displays[8].clearDisplay() ;
  displays[8].setTextColor(SSD1306_WHITE) ;
  int16_t x1, y1 ; uint16_t w, h ;

  if (p == 0) {
    // Page 0: LINE IN header + volume bar (same as updateVolumeDisplay)
    drawLineInHeading();
    int barFill = map(function_value[8], 0, 127, 0, 122) ;
    displays[8].drawRect(2, 11, 124, 8, SSD1306_WHITE) ;
    if (barFill > 0) { displays[8].fillRect(3, 12, barFill, 6, SSD1306_WHITE) ; }
  } else if (p == 1) {
    // Page 1: VIDEO OUT header + now playing
    displays[8].setTextSize(1) ;
    const char* heading=clipMenu.active<0?"VIDEO OUT":clipMenu.names[clipMenu.active];
    displays[8].getTextBounds(heading, 0, 0, &x1, &y1, &w, &h) ;
    displays[8].setCursor((128 - w) / 2, 1) ; displays[8].println(heading) ;
    displays[8].getTextBounds(nowPlaying, 0, 0, &x1, &y1, &w, &h) ;
    displays[8].setCursor(constrain((128 - w) / 2, 2, 128), 10) ; displays[8].println(nowPlaying) ;
    // play/pause icon
    if (isPlaying) { displays[8].fillTriangle(2, 19, 2, 27, 9, 23, SSD1306_WHITE) ; }
    else { displays[8].fillRect(2, 19, 3, 9, SSD1306_WHITE) ; displays[8].fillRect(7, 19, 3, 9, SSD1306_WHITE) ; }
    displays[8].drawFastHLine(2, 29, 124, SSD1306_WHITE) ;
    drawClipMenuRows();
  } else if (p == 2) {
    // Page 2: mixer display
    drawMixerDisplay8Content() ;
  }
  flushDisplay(8) ;
}

void drawPatchDisplay() {
  displays[5].clearDisplay() ;
  drawMenuHeader(5, "VIDEO PATCHES") ;
  int16_t x1, y1 ; uint16_t w, h ;

  if(patchLoadPending && patchTransactionOperation[0]=='V'){
    displays[5].setTextSize(2);displays[5].setCursor(20,26);displays[5].println((recallQueueFullAt && millis()-recallQueueFullAt<1000)?"BUSY":patchTransactionUncertain?"WAIT":(strcmp(patchTransactionOperation+1,"SAVE")==0?"SAVING":"LOADING"));flushDisplay(5);page2Updated[P2_PATCH]=true;return;
  }
  if (patchFlashActive) {
    const char* msg = patchFlashError ? "ERR" : (patchFlashSaved ? "SAVED!" : "LOADED!") ;
    displays[5].setTextSize(2) ;
    displays[5].getTextBounds(msg, 0, 0, &x1, &y1, &w, &h) ;
    displays[5].setCursor((128 - w) / 2, 26) ;
    displays[5].println(msg) ;
    flushDisplay(5) ;
    page2Updated[P2_PATCH] = true ;
    return ;
  }

  for (int i = 0 ; i < MENU_VISIBLE_ROWS ; i++) {
    int idx  = patchScroll + i ;
    if (idx >= autoeyez::videoPatchSlotCount) break ;
    int yPos = menuRowY(i) ;
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
  flushDisplay(5) ;
  page2Updated[P2_PATCH] = true ;
}

void drawAudioPatchDisplay() {
  displays[6].clearDisplay() ;
  drawMenuHeader(6, "AUDIO PATCHES") ;
  int16_t x1, y1 ; uint16_t w, h ;

  if(patchLoadPending && patchTransactionOperation[0]=='A'){
    displays[6].setTextSize(2);displays[6].setCursor(20,26);displays[6].println((recallQueueFullAt && millis()-recallQueueFullAt<1000)?"BUSY":patchTransactionUncertain?"WAIT":(strcmp(patchTransactionOperation+1,"SAVE")==0?"SAVING":"LOADING"));flushDisplay(6);page2Updated[P2_AUDIO]=true;return;
  }
  if (audioPatchFlashActive) {
    const char* msg = audioPatchFlashError ? "ERR" : (audioPatchFlashSaved ? "SAVED!" : "LOADED!") ;
    displays[6].setTextSize(2) ;
    displays[6].getTextBounds(msg, 0, 0, &x1, &y1, &w, &h) ;
    displays[6].setCursor((128 - w) / 2, 26) ;
    displays[6].println(msg) ;
    flushDisplay(6) ;
    page2Updated[P2_AUDIO] = true ;
    return ;
  }

  for (int i = 0 ; i < MENU_VISIBLE_ROWS ; i++) {
    int idx = audioPatchScroll + i ;
    if (idx >= autoeyez::audioPatchSlotCount) break ;
    int yPos = menuRowY(i) ;
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
  flushDisplay(6) ;
  page2Updated[P2_AUDIO] = true ;
}

void drawMoshDisplay() {
  displays[7].clearDisplay() ;
  displays[7].setTextColor(SSD1306_WHITE) ;
  int16_t x1, y1 ; uint16_t w, h ;

  const int values[ENC7_MODE_COUNT] = {
    codecLevel, codecStatic,
    react_band < 0 ? chromaValue : postFxReactiveValues[react_band][1],
    react_band < 0 ? crushValue : postFxReactiveValues[react_band][2],
    react_band < 0 ? pixelValue : postFxReactiveValues[react_band][3],
    react_band < 0 ? bleedValue : processingReactiveValues[react_band][0],
    react_band < 0 ? alignValue : processingReactiveValues[react_band][1],
    react_band < 0 ? ghostValue : processingReactiveValues[react_band][2],
    react_band < 0 ? wobbleValue : processingReactiveValues[react_band][3],
    react_band < 0 ? overdriveValue : processingReactiveValues[react_band][4],
    react_band < 0 ? holdValue : holdReactiveValues[react_band],
    react_band < 0 ? peekValue : peekReactiveValues[react_band],
    peekMaskModeValue ? 127 : 0,
    codecLow, codecMid, codecHigh, codecRate } ;
  if (!postFxEditing) {
    drawMenuHeader(7, react_band < 0 ? "PROCESSING" :
        react_band == 0 ? "PROCESS LOW" : react_band == 1 ? "PROCESS MID" : "PROCESS HIGH") ;
    for (int i = 0 ; i < MENU_VISIBLE_ROWS ; i++) {
      int idx = postFxScroll + i ;
      if (idx >= ENC7_MODE_COUNT) break ;
      int yPos = menuRowY(i) ;
      if (idx == enc7Mode) {
        displays[7].fillRect(2, yPos - 1, 124, 11, SSD1306_WHITE) ;
        displays[7].setTextColor(SSD1306_BLACK) ;
      } else displays[7].setTextColor(SSD1306_WHITE) ;
      displays[7].setCursor(6, yPos) ;
      displays[7].print(enc7ModeLabels[idx]) ;
      char valueBuf[16] ;
      if (idx == 12) snprintf(valueBuf, sizeof(valueBuf), "%s", peekMaskModeValue ? "LUMA" : "ALPHA") ;
      else snprintf(valueBuf, sizeof(valueBuf), "%d%%", values[idx] * 100 / 127) ;
      displays[7].getTextBounds(valueBuf, 0, 0, &x1, &y1, &w, &h) ;
      displays[7].setCursor(122 - w, yPos) ;
      displays[7].print(valueBuf) ;
    }
  } else {
    int currentValue = values[enc7Mode] ;
    const char* label = enc7ModeLabels[enc7Mode] ;
    displays[7].setTextSize(strlen(label) > 9 ? 1 : 2) ;
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
    if (enc7Mode == 12) snprintf(valueBuf, sizeof(valueBuf), "%s  PUSH FOR LIST", peekMaskModeValue ? "LUMA" : "ALPHA") ;
    else snprintf(valueBuf, sizeof(valueBuf), "%d%%  PUSH FOR LIST", currentValue * 100 / 127) ;
    displays[7].getTextBounds(valueBuf, 0, 0, &x1, &y1, &w, &h) ;
    displays[7].setCursor((128 - w) / 2, 49) ;
    displays[7].println(valueBuf) ;
  }
  displays[7].setTextColor(SSD1306_WHITE) ;
  flushDisplay(7) ;
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
  const char* heading=clipMenu.active<0?"VIDEO OUT":clipMenu.names[clipMenu.active];
  displays[8].getTextBounds(heading, 0, 0, &x1, &y1, &w, &h) ;
  displays[8].setCursor((128 - w) / 2, 1) ; displays[8].println(heading) ;
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
  drawClipMenuRows();
  flushDisplay(8) ;
  videoDisplayUpdated = true ;
}

// ============================================================
// SPECTRUM + VOLUME + FPS
// ============================================================

void updateVolumeDisplay() {
  displays[8].fillRect(2, 0, 124, 22, SSD1306_BLACK) ;
  drawLineInHeading();
  int barFill = map(function_value[8], 0, 127, 0, 122) ;
  displays[8].drawRect(2, 11, 124, 8, SSD1306_WHITE) ;
  if (barFill > 0) { displays[8].fillRect(3, 12, barFill, 6, SSD1306_WHITE) ; }
  flushDisplay(8) ;
}

void drawSpectrum() {
  if (page == 0 && !enc8HeldLong && pendingPage < 0 && !shaderOverlayActive) {
    if (spectrumGeneration != fftSnapshot.generation) {
      spectrumGeneration=fftSnapshot.generation;
      displays[8].fillRect(0, 22, 128, 42, SSD1306_BLACK) ;
      spectrum.update([](unsigned bin) { return readControllerFft(bin); },
                      autoeyez::lineInGain(function_value[8]), millis());
      for (int i = 0; i < 32; ++i) {
        const int barHeight = constrain((int)(spectrum.levels[i] * 32), 0, 32);
        const int peakHeight = constrain((int)(spectrum.peaks[i] * 32), 0, 32);
        if (barHeight > 0) displays[8].fillRect(1 + i * 4, 55 - barHeight, 3, barHeight, SSD1306_WHITE);
        if (peakHeight > barHeight) displays[8].drawFastHLine(1 + i * 4, 55 - peakHeight, 3, SSD1306_WHITE);
      }
      // Labels mark approximate bin centers, not equal-width frequency steps.
      displays[8].setTextSize(1); displays[8].setTextColor(SSD1306_WHITE);
      displays[8].setCursor(15, 56); displays[8].print("1k");
      displays[8].setCursor(54, 56); displays[8].print("4k");
      displays[8].setCursor(90, 56); displays[8].print("10k");
      flushDisplay(8) ;
    }
  }
}

// =============================================================================
// PROCESS FFT BANDS AND SEND MIDI
// =============================================================================
// Calculates low/mid/high band energies from FFT and sends as MIDI CCs.
// Uses asymmetric smoothing (fast attack, slow decay) for punchy response.
// Gain controlled by encoder 8 (function_value[8]) — same as spectrum display.

bool sendControllerFft(const char* message){
  if(Serial.availableForWrite()<int(strlen(message)+2))return false;
  Serial.println(message);return true;
}

void sendFftFrame(int low, int mid, int high,
                  int extLow, int extMid, int extHigh) {
  static uint8_t sequence = 0;
  usbMIDI.sendControlChange(CC_BAND_LOW, low, 1);
  usbMIDI.sendControlChange(CC_BAND_MID, mid, 1);
  usbMIDI.sendControlChange(CC_BAND_HIGH, high, 1);
  const int extended[3] = {extLow, extMid, extHigh};
  for (int i = 0; i < 3; ++i) {
    usbMIDI.sendControlChange(CC_BAND_EXT_FIRST + i * 2,
                              extended[i] >> 7, 1);
    usbMIDI.sendControlChange(CC_BAND_EXT_FIRST + i * 2 + 1,
                              extended[i] & 0x7f, 1);
  }
  usbMIDI.sendControlChange(CC_BAND_FRAME, sequence++ & 0x7f, 1);
  usbMIDI.send_now();
#if FFT_SERIAL_COMPAT
  char fftBuf[24];
  snprintf(fftBuf, sizeof(fftBuf), "FFT:%d,%d,%d", low, mid, high);
  sendControllerFft(fftBuf);
#endif
  lastBandLowCC = low;
  lastBandMidCC = mid;
  lastBandHighCC = high;
  for (int i = 0; i < 3; ++i) lastBandExt[i] = extended[i];
  lastBandSendTime = millis();
}

void processFftBands() {
  // Band and spectrum consumers share the newest captured FFT frame.
  // A 10ms change gate carries each available trial FFT snapshot; the 100ms
  // unchanged-value heartbeat remains below.

  // Get input gain multiplier from encoder 8 (line-in volume)
  // Same scaling as drawSpectrum uses
  float gain = autoeyez::lineInGain(function_value[8]);

  // If gain is zero, zero out bands and send zeros (once)
  if (gain < 0.01f) {
    if (lastBandLowCC != 0 || lastBandMidCC != 0 || lastBandHighCC != 0 ||
        lastBandExt[0] != 0 || lastBandExt[1] != 0 || lastBandExt[2] != 0) {
      bandLow = bandMid = bandHigh = 0.0f ;
      sendFftFrame(0, 0, 0, 0, 0, 0);
    }
    return ;
  }

  const auto bands = controllerBands(gain);
  float rawLow = bands.low, rawMid = bands.mid, rawHigh = bands.high;

  // Apply asymmetric smoothing (fast attack, slow decay)
  float rateLow  = (rawLow  > bandLow)  ? bandAttackRate : bandDecayRate ;
  float rateMid  = (rawMid  > bandMid)  ? bandAttackRate : bandDecayRate ;
  float rateHigh = (rawHigh > bandHigh) ? bandAttackRate : bandDecayRate ;

  bandLow  += (rawLow  - bandLow)  * rateLow ;
  bandMid  += (rawMid  - bandMid)  * rateMid ;
  bandHigh += (rawHigh - bandHigh) * rateHigh ;

  // Clamp raw values to 0-1 range, then convert to MIDI CC values.
  float clampedLow  = constrain(rawLow,  0.0f, 1.0f) ;
  float clampedMid  = constrain(rawMid,  0.0f, 1.0f) ;
  float clampedHigh = constrain(rawHigh, 0.0f, 1.0f) ;
  int ccLow  = (int)(clampedLow  * 127.0f) ;
  int ccMid  = (int)(clampedMid  * 127.0f) ;
  int ccHigh = (int)(clampedHigh * 127.0f) ;
  auto extended = [](float value) {
    return int(constrain(value, 0.0f, BAND_EXT_MAX) *
               (16383.0f / BAND_EXT_MAX) + 0.5f);
  };
  const int extLow = extended(rawLow), extMid = extended(rawMid),
            extHigh = extended(rawHigh);

  // Time-based rate limiting
  unsigned long now = millis() ;
  if (now - lastBandSendTime < BAND_SEND_INTERVAL_MS) return ;

  // Send changed values promptly, with a heartbeat when values hold steady.
  bool changed = (abs(ccLow - lastBandLowCC) > BAND_CC_THRESHOLD) ||
                 (abs(ccMid - lastBandMidCC) > BAND_CC_THRESHOLD) ||
                 (abs(ccHigh - lastBandHighCC) > BAND_CC_THRESHOLD) ||
                 abs(extLow - lastBandExt[0]) > 1 ||
                 abs(extMid - lastBandExt[1]) > 1 ||
                 abs(extHigh - lastBandExt[2]) > 1 ;

  if (changed) {
    sendFftFrame(ccLow, ccMid, ccHigh, extLow, extMid, extHigh);
  } else if (now - lastBandSendTime > 100) {
    // Heartbeat: force send every 100ms even if unchanged
    sendFftFrame(ccLow, ccMid, ccHigh, extLow, extMid, extHigh);
  }
}

// ============================================================
// MIDI
// ============================================================

void initializeAllMidiControls() {
  for (const auto& parameter : autoeyez::initialParameters) {
    usbMIDI.sendControlChange(parameter.control, parameter.value, 1) ;
    if (parameter.control == 16 || parameter.control % 8 == 0) {
      usbMIDI.send_now() ; delayMicroseconds(500) ;
    }
  }
  usbMIDI.send_now() ;
  usbMIDI.sendControlChange(94, 0, 1); usbMIDI.send_now();
  sendCodecSettings();
}

bool sendAudioPatchCommand(uint8_t control, int slot) {
  if ((control != 92 && control != 93) || !autoeyez::validControllerAudioSlot(slot)) {
    Serial.println("PATCH_ERROR:INVALID_AUDIO_SLOT") ;
    return false ;
  }
  usbMIDI.sendControlChange(control, slot + 1, 1) ; usbMIDI.send_now() ;
  return true ;
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
  for (int slot = 0 ; slot < autoeyez::videoPatchSlotCount ; slot++) {
    char finalName[24], backupName[24] ;
    snprintf(finalName, sizeof(finalName), "/PATCH%02d.TXT", slot + 1) ;
    snprintf(backupName, sizeof(backupName), "/PATCH%02d.BAK", slot + 1) ;
    if (!autoeyez::recoverPatch(SD, finalName, backupName)) {
      Serial.println("SD_ERROR:PATCH_RECOVERY") ;
    }
  }

  for (int i = 0 ; i < autoeyez::videoPatchSlotCount ; i++) {
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
    if (!autoeyez::validControllerVideoSlot(slot)) continue ;
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

}

void scanAudioPatches() {
  for (int slot = 0 ; slot < autoeyez::audioPatchSlotCount ; slot++) {
    char finalName[24], backupName[24] ;
    snprintf(finalName, sizeof(finalName), "/AUDIO%02d.TXT", slot + 1) ;
    snprintf(backupName, sizeof(backupName), "/AUDIO%02d.BAK", slot + 1) ;
    if (!autoeyez::recoverPatch(SD, finalName, backupName)) {
      Serial.println("SD_ERROR:AUDIO_RECOVERY") ;
    }
  }

  for (int i = 0 ; i < autoeyez::audioPatchSlotCount ; i++) {
    snprintf(audioPatchNames[i], 10, "AUDIO %02d", i + 1) ;
    audioPatchFilled[i] = false ;
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
    if (strncmp(fname, "AUDIO", 5) != 0) continue ;
    if (strcmp(fname + flen - 4, ".TXT") != 0) continue ;
    char numBuf[3] = { fname[5], fname[6], '\0' } ;
    int slot = atoi(numBuf) - 1 ;
    if (!autoeyez::validControllerAudioSlot(slot)) continue ;
    audioPatchFilled[slot] = true ;
  }
  root.close() ;

}

/**
 * Save current state to a patch slot on SD card.
 * Patch files are plain text with key=value pairs.
 * Called when user holds encoder 5 button on page 2.
 *
 * @param slot Patch slot number (0-63)
 */
bool savePatch(int slot) {
  if (patchLoadPending || !autoeyez::validControllerVideoSlot(slot)) return false ;
  char finalName[24], tempName[24], backupName[24] ;
  snprintf(finalName, sizeof(finalName), "/PATCH%02d.TXT", slot + 1) ;
  snprintf(tempName, sizeof(tempName), "/PATCH%02d.TMP", slot + 1) ;
  snprintf(backupName, sizeof(backupName), "/PATCH%02d.BAK", slot + 1) ;
  char data[1536] ;
  size_t used = 0 ;
  bool dataOK = true ;
  auto append = [&](const char* key, int value) {
    if (!autoeyez::appendPatchLine(data, sizeof(data), used, key, value)) dataOK = false ;
  } ;
  append("patchVersion",7);append("slot",slot);
  // Save encoder values (18 parameters across 2 pages)
  for (int i = 0 ; i < 18 ; i++) { char key[12] ; snprintf(key, sizeof(key), "enc%d", i) ; append(key, function_value[i]) ; }

  // Save toggle states (8 on/off controls)
  for (int i = 0 ; i < 8 ; i++)  { char key[12] ; snprintf(key, sizeof(key), "ctrl%d", i) ; append(key, control[i] ? 1 : 0) ; }

  // Save function multiplier states (8 encoders × 4 levels each)
  for (int i = 0 ; i < 8 ; i++)  { char key[12] ; snprintf(key, sizeof(key), "func%d", i) ; append(key, function[i]) ; }
  append("react", react_band) ;
  append("lumaKey", lumaKeyValue) ;
  append("lumaHigh", lumaHighValue) ;
  append("lumaLowEn", lumaLowEnabled ? 1 : 0) ;
  append("lumaHiEn", lumaHighEnabled ? 1 : 0) ;
  append("renderVersion", 1);
  append("renderLumaInvert", rendererLumaInvert ? 1 : 0);
  append("renderMirror", rendererMirror ? 1 : 0);
  append("mix", mixValue) ;
  append("chA", chASource) ;
  append("chB", chBSource) ;
  append("lumaSrc", lumaSourceSel) ;
  append("lumaTrack", lumaTrackEnabled ? 1 : 0) ;
  append("mixMode", mixerSubMode) ;
  append("clip", clipPos) ;
  append("loop", isLooping ? 1 : 0) ;
  append("srcHue", srcHue) ;
  append("srcSat", srcSat) ;
  append("srcVal", srcVal) ;
  append("srcShader", srcShader) ;
  append("mosh", 0) ;
  append("codecLevel", codecLevel) ;
  append("codecStatic", codecStatic) ;
  append("codecLow", codecLow) ;
  append("codecMid", codecMid) ;
  append("codecHigh", codecHigh) ;
  append("codecRate", codecRate) ;
  append("chroma", chromaValue) ;
  append("crush", crushValue) ;
  append("pixel", pixelValue) ;
  append("bleed", bleedValue) ;
  append("align", alignValue) ;
  append("ghost", ghostValue) ;
  append("wobble", wobbleValue) ;
  append("overdrive", overdriveValue) ;
  append("hold", holdValue) ;
  append("peek", peekValue) ;
  append("peekMask", peekMaskModeValue ? 1 : 0) ;
  append("houseCard", houseCardEnabled ? 1 : 0) ;
  append("houseLogo", houseLogoEnabled ? 1 : 0) ;
  append("mainLogo", annLogoEnabled ? 1 : 0) ;
  if (!dataOK) { Serial.println("SD_ERROR:PATCH_TOO_LARGE") ; return false ; }
  autoeyez::VideoPatch snapshot;
  char parsed[1536];memcpy(parsed,data,used);parsed[used]=0;
  for(char* line=strtok(parsed,"\r\n");line;line=strtok(nullptr,"\r\n"))if(!snapshot.line(line)){Serial.println("PATCH_ERROR:INVALID_SNAPSHOT");return false;}
  if(!snapshot.finish(slot)){Serial.println("PATCH_ERROR:INCOMPLETE_SNAPSHOT");return false;}
  patchSaveHadPrevious=SD.exists(finalName);
  beginPatchTransaction("VSAVE",slot,&snapshot);
  if(!recordPendingSave(data,used)){patchLoadPending=false;return false;}
  const auto result = autoeyez::saveVerifiedPatch(SD, finalName, tempName, backupName,
                                                 data, used, FILE_WRITE) ;
  if (result != autoeyez::SaveResult::Saved) {
    Serial.print("SD_ERROR:PATCH_SAVE:") ; Serial.println(static_cast<int>(result)) ;
    patchLoadPending=false;erasePatchJournal();return false ;
  }
  return true;
}

bool saveAudioMarker(int slot) {
  if (!autoeyez::validControllerAudioSlot(slot)) return false ;
  char finalName[24], tempName[24], backupName[24] ;
  snprintf(finalName, sizeof(finalName), "/AUDIO%02d.TXT", slot + 1) ;
  snprintf(tempName, sizeof(tempName), "/AUDIO%02d.TMP", slot + 1) ;
  snprintf(backupName, sizeof(backupName), "/AUDIO%02d.BAK", slot + 1) ;
  const char marker[] = "marker=1\r\n" ;
  return autoeyez::saveVerifiedPatch(SD, finalName, tempName, backupName,
                                    marker, sizeof(marker)-1, FILE_WRITE) == autoeyez::SaveResult::Saved ;
}

/**
 * Load a patch from SD card and apply to current state.
 * Parses the key=value pairs and updates all parameters.
 * Sends MIDI and serial commands to sync autowaaave and autoclip.
 * Called when user taps encoder 5 button on page 2 (if slot is filled).
 *
 * @param slot Patch slot number (0-63)
 */
void loadPatch(int slot) {
  if(!autoeyez::validControllerVideoSlot(slot))return;
  if(patchLoadPending){if(!queuedRecalls.push(false,slot)){recallQueueFullAt=millis();Serial.println("PATCH_ERROR:RECALL_QUEUE_FULL");page2Updated[P2_PATCH]=false;}return;}
  char fname[24],backup[24];snprintf(fname,sizeof(fname),"/PATCH%02d.TXT",slot+1);snprintf(backup,sizeof(backup),"/PATCH%02d.BAK",slot+1);
  bool valid=autoeyez::recoverPatch(SD,fname,backup);File file=SD.open(fname);
  autoeyez::VideoPatch snapshot;char line[64];size_t used=0;
  if(!file||file.size()>1535)valid=false;
  while(valid && file.available()){
    char c=file.read();
    if(c=='\n') {line[used]=0;if(used && !snapshot.line(line))valid=false;used=0;}
    else if(c=='\r'){if(file.available()&&file.peek()!='\n'){valid=false;break;}}
    else {if(used>=sizeof(line)-1){valid=false;break;}line[used++]=c;}
  }
  line[used]=0;if(used && !snapshot.line(line))valid=false;file.close();
  if(!valid||!snapshot.finish(slot)){
    Serial.println("PATCH_ERROR:INVALID_FILE");patchFlashActive=true;patchFlashSaved=false;patchFlashError=true;patchFlashTimer=millis();page2Updated[P2_PATCH]=false;return;
  }
  beginPatchTransaction("VLOAD",slot,&snapshot);
}

// One-time remote recovery path. Keep the original .BAK and a second .RCV
// copy; move the reset-era .TXT to .BAD only after the replacement is verified.
void restorePatchBackup(int slot) {
  if (!autoeyez::validControllerVideoSlot(slot) || patchLoadPending) {
    Serial.println("PATCH_BACKUP_ERROR:BUSY_OR_SLOT") ; return ;
  }
  char finalName[24], backupName[24], safetyName[24], tempName[24], badName[24] ;
  snprintf(finalName,sizeof(finalName),"/PATCH%02d.TXT",slot+1) ;
  snprintf(backupName,sizeof(backupName),"/PATCH%02d.BAK",slot+1) ;
  snprintf(safetyName,sizeof(safetyName),"/PATCH%02d.RCV",slot+1) ;
  snprintf(tempName,sizeof(tempName),"/PATCH%02d.TMP",slot+1) ;
  snprintf(badName,sizeof(badName),"/PATCH%02d.BAD",slot+1) ;
  File backup=SD.open(backupName) ;
  char data[1536] ;
  const size_t length=backup ? backup.size() : 0 ;
  if (!backup || !length || length >= sizeof(data) ||
      backup.read(reinterpret_cast<uint8_t*>(data),length) != int(length)) {
    backup.close() ; Serial.println("PATCH_BACKUP_ERROR:READ") ; return ;
  }
  backup.close() ;
  autoeyez::VideoPatch snapshot ;
  char parsed[1536] ; memcpy(parsed,data,length) ; parsed[length]=0 ;
  bool valid=true ;
  for (char* line=strtok(parsed,"\r\n");line;line=strtok(nullptr,"\r\n"))
    if (!snapshot.line(line)) { valid=false ; break ; }
  if (!valid || !snapshot.finish(slot)) {
    Serial.println("PATCH_BACKUP_ERROR:INVALID_PATCH") ; return ;
  }
  // Keep every previous displaced patch. A second recovery must not
  // overwrite the first .BAD copy or block an otherwise valid restore.
  if (SD.exists(badName)) {
    bool found=false ;
    for (int copy=2;copy<=9;copy++) {
      snprintf(badName,sizeof(badName),"/PATCH%02d.BD%d",slot+1,copy) ;
      if (!SD.exists(badName)) { found=true ; break ; }
    }
    if (!found) {
      Serial.println("PATCH_BACKUP_ERROR:NO_FREE_BAD_COPY") ; return ;
    }
  }
  if (SD.exists(safetyName) && !autoeyez::verifyPatch(SD,safetyName,data,length)) {
    bool found=false ;
    for (int copy=2;copy<=9;copy++) {
      snprintf(safetyName,sizeof(safetyName),"/PATCH%02d.RV%d",slot+1,copy) ;
      if (!SD.exists(safetyName) || autoeyez::verifyPatch(SD,safetyName,data,length)) {
        found=true ; break ;
      }
    }
    if (!found) {
      Serial.println("PATCH_BACKUP_ERROR:NO_FREE_SAFETY_COPY") ; return ;
    }
  }
  if (!SD.exists(safetyName)) {
    File safety=SD.open(safetyName,FILE_WRITE) ;
    const bool written=safety && safety.write(reinterpret_cast<const uint8_t*>(data),length)==length ;
    if (safety) { safety.flush() ; safety.close() ; }
    if (!written || !autoeyez::verifyPatch(SD,safetyName,data,length)) {
      Serial.println("PATCH_BACKUP_ERROR:SAFETY_COPY") ; return ;
    }
  }
  if (SD.exists(tempName) && !SD.remove(tempName)) {
    Serial.println("PATCH_BACKUP_ERROR:TEMP_REMOVE") ; return ;
  }
  File temp=SD.open(tempName,FILE_WRITE) ;
  const bool written=temp && temp.write(reinterpret_cast<const uint8_t*>(data),length)==length ;
  if (temp) { temp.flush() ; temp.close() ; }
  if (!written || !autoeyez::verifyPatch(SD,tempName,data,length)) {
    Serial.println("PATCH_BACKUP_ERROR:TEMP_VERIFY") ; return ;
  }
  if (!SD.exists(finalName) || !SD.rename(finalName,badName)) {
    Serial.println("PATCH_BACKUP_ERROR:CURRENT_RENAME") ; return ;
  }
  if (!SD.rename(tempName,finalName) ||
      !autoeyez::verifyPatch(SD,finalName,data,length)) {
    if (SD.exists(finalName)) SD.remove(finalName) ;
    if (!SD.rename(badName,finalName))
      Serial.println("PATCH_BACKUP_ERROR:ROLLBACK_FAILED") ;
    else Serial.println("PATCH_BACKUP_ERROR:RESTORE_FAILED") ;
    return ;
  }
  Serial.print("PATCH_BACKUP_RESTORED:") ; Serial.println(slot+1) ;
  loadPatch(slot) ;
}

void beginPatchTransaction(const char* operation,int slot,const autoeyez::VideoPatch* patch){
  if(patchLoadPending)return;
  static uint32_t sequence=0;
  static const uint32_t bootNonce=ARM_DWT_CYCCNT ^ micros() ^ uint32_t(random(1,2147483647));
  snprintf(patchTransactionId,sizeof(patchTransactionId),"%08lx%08lx",(unsigned long)bootNonce,(unsigned long)++sequence);
  strncpy(patchTransactionOperation,operation,sizeof(patchTransactionOperation)-1);patchTransactionSlot=slot;
  if(patch)pendingVideoPatch=*patch;
  patchLoadPending=true;patchTransactionUncertain=false;patchRecoveringSave=false;patchRecoverUndo=false;patchTransactionSent=millis()-501;
  page2Updated[P2_PATCH]=false;page2Updated[P2_AUDIO]=false;
}

void executePatchLoad(){
  if(!patchLoadPending){autoeyez::Recall request;if(queuedRecalls.pop(request)){if(request.audio)beginPatchTransaction("ALOAD",request.slot,nullptr);else loadPatch(request.slot);}}
  static unsigned long sourceSyncSent=0;if(!sourceStateSynced&&millis()-sourceSyncSent>500){Serial.println("SOURCE_STATE?");sourceSyncSent=millis();}
  if(!patchLoadPending || millis()-patchTransactionSent<500)return;
  patchTransactionSent=millis();
  if(patchRecoveringSave){Serial.print(patchRecoverUndo?"PTX_UNDO:":"PTX_RECOVER:");Serial.println(patchTransactionId);return;}
  Serial.print("PTX:");Serial.print(patchTransactionId);Serial.print(':');Serial.print(patchTransactionOperation);Serial.print(':');Serial.print(patchTransactionSlot);
  if(patchTransactionOperation[0]=='V'){
    Serial.print(':');for(size_t i=0;i<autoeyez::patchFieldCount;++i){if(i)Serial.print(',');Serial.print(pendingVideoPatch.values[i]);}
  }
  Serial.println();
}

void finishPatchTransaction(const char* status){
  if(strcmp(status,"OK")!=0 && strcmp(status,"ERROR")!=0 && strcmp(status,"UNCERTAIN")!=0)return;
  if(strcmp(status,"UNCERTAIN")==0){patchTransactionUncertain=true;page2Updated[P2_PATCH]=false;page2Updated[P2_AUDIO]=false;return;}
  const bool okay=strcmp(status,"OK")==0;
  if(okay && patchRecoveringSave){
    char path[24];snprintf(path,sizeof(path),patchTransactionOperation[0]=='V'?"/PATCH%02d.TXT":"/AUDIO%02d.TXT",patchTransactionSlot+1);
    File saved=SD.open(path);uint32_t hash=2166136261u;while(saved&&saved.available()){uint8_t byte=saved.read();hash=autoeyez::patchChecksum(&byte,1,hash);}bool valid=bool(saved)&&hash==pendingSave.newChecksum;saved.close();
    if(!valid){patchRecoverUndo=true;patchTransactionUncertain=true;return;}
  }
  const bool saving=strcmp(patchTransactionOperation+1,"SAVE")==0;
  if(okay && strcmp(patchTransactionOperation,"VLOAD")==0){
    for(int i=0;i<18;++i){char key[12];snprintf(key,sizeof(key),"enc%d",i);function_value[i]=pendingVideoPatch.get(key);}
    for(int i=0;i<8;++i){char key[12];snprintf(key,sizeof(key),"ctrl%d",i);control[i]=pendingVideoPatch.get(key);}
    for(int i=0;i<8;++i){char key[12];snprintf(key,sizeof(key),"func%d",i);function[i]=pendingVideoPatch.get(key);}
    react_band=pendingVideoPatch.get("react");
    Serial.print("SHADER_BAND:");Serial.println(react_band);
    // A loaded patch becomes the new held bar position on both graph pages.
    for(int i=0;i<18;++i)saved_function_value[i]=function_value[i];
    lumaKeyValue=pendingVideoPatch.get("lumaKey");
    lumaHighValue=pendingVideoPatch.get("lumaHigh");
    lumaLowEnabled=pendingVideoPatch.get("lumaLowEn");
    lumaHighEnabled=pendingVideoPatch.get("lumaHiEn");
    rendererLumaInvert=pendingVideoPatch.get("renderLumaInvert");
    rendererMirror=pendingVideoPatch.get("renderMirror");
    mixValue=pendingVideoPatch.get("mix");
    chASource=pendingVideoPatch.get("chA");
    chBSource=pendingVideoPatch.get("chB");
    lumaSourceSel=pendingVideoPatch.get("lumaSrc");
    lumaTrackEnabled=pendingVideoPatch.get("lumaTrack");
    mixerSubMode=pendingVideoPatch.get("mixMode");
    clipPos=pendingVideoPatch.get("clip");
    isLooping=pendingVideoPatch.get("loop");
    srcHue=pendingVideoPatch.get("srcHue");
    srcSat=pendingVideoPatch.get("srcSat");
    srcVal=pendingVideoPatch.get("srcVal");
    srcShader=pendingVideoPatch.get("srcShader");
    codecLevel=pendingVideoPatch.get("codecLevel");
    codecStatic=pendingVideoPatch.get("codecStatic");
    codecLow=pendingVideoPatch.get("codecLow");
    codecMid=pendingVideoPatch.get("codecMid");
    codecHigh=pendingVideoPatch.get("codecHigh");
    codecRate=pendingVideoPatch.get("codecRate");
    sendCodecSettings();
    usbMIDI.sendControlChange(94, 0, 1); usbMIDI.send_now();
    chromaValue=pendingVideoPatch.get("chroma");
    crushValue=pendingVideoPatch.get("crush");
    pixelValue=pendingVideoPatch.get("pixel");
    bleedValue=pendingVideoPatch.get("bleed");
    alignValue=pendingVideoPatch.get("align");
    ghostValue=pendingVideoPatch.get("ghost");
    wobbleValue=pendingVideoPatch.get("wobble");
    overdriveValue=pendingVideoPatch.get("overdrive");
    holdValue=pendingVideoPatch.get("hold");
    peekValue=pendingVideoPatch.get("peek");
    peekMaskModeValue=pendingVideoPatch.get("peekMask");
    houseCardEnabled=pendingVideoPatch.get("houseCard");
    houseLogoEnabled=pendingVideoPatch.get("houseLogo");
    annLogoEnabled=pendingVideoPatch.get("mainLogo");
    sourceConfigMenu.editing=false;mixValueSmoothed=mixValue;chASelection=chASource;chBSelection=chBSource;lumaSourceCursor=lumaSourceSel;
    for(int i=0;i<9;++i){labelUpdated[i]=false;graphUpdated[i]=false;}
    for(int i=0;i<4;++i){controlUpdated[i]=false;functionUpdated[i]=false;}
    for(int i=0;i<P2_COUNT;++i)page2Updated[i]=false;
  }
  if(!okay && saving && !restorePendingSave()){
    patchTransactionUncertain=true;Serial.println("SD_ERROR:TRANSACTION_RESTORE");return;
  }
  if(saving)erasePatchJournal();
  if(patchTransactionOperation[0]=='V'){
    if(okay&&saving){patchFilled[patchTransactionSlot]=true;snprintf(patchNames[patchTransactionSlot],10,"PATCH %02d",patchTransactionSlot+1);}
    patchFlashActive=true;patchFlashSaved=saving;patchFlashError=!okay;patchFlashTimer=millis();page2Updated[P2_PATCH]=false;
  }else{
    if(okay&&saving)audioPatchFilled[patchTransactionSlot]=true;
    if(okay){
      Serial.print(saving?"SHADER_AUDIO_SAVE:":"SHADER_AUDIO_LOAD:");
      Serial.println(patchTransactionSlot);
    }
    audioPatchFlashActive=true;audioPatchFlashSaved=saving;audioPatchFlashError=!okay;audioPatchFlashTimer=millis();page2Updated[P2_AUDIO]=false;page2Updated[P2_FFT_SMOOTH]=false;
  }
  patchLoadPending=false;
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
  fft.averageTogether(4) ;    // trial: halve the library's default 8-result averaging
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
    displays[i].clearDisplay() ; flushDisplay(i) ;
    updateLabel(i, boot_label[i], 4) ;
  }
  delay(50) ;
  SD.begin(SD_CS) ;
  scanPatches() ;
  recoverPendingSave();
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
  // The normal-page widgets only redraw portions of an OLED. Clear every
  // framebuffer before the draw queue starts so no page-3 menu pixels remain.
  // The existing OLED budget still sends at most one screen per loop.
  oledBudget.dirty = 0 ;
  for (int i = 0 ; i < 9 ; i++) displays[i].clearDisplay() ;
  page = newPage ;
  fftCooldownUntil = millis() + FFT_COOLDOWN_MS ;  // pause FFT MIDI during page transition
  invalidateAllDisplays() ;
  // Reset encoder accumulators and resync positions on page switch
  enc0Accum = 0 ;
  enc1Accum = 0 ;
  enc4Accum = 0 ;
  enc7Accum = 0 ;
  enc8Accum = 0 ;
  lineInGainAccum = 0 ;
  lastEncPos[0]  = encoders[0]->read() ;
  lastEncPos[1]  = encoders[1]->read() ;
  lastEncPos[4]  = encoders[4]->read() ;
  lastEncPos[7]  = encoders[7]->read() ;
  lastEncPos[8]  = encoders[8]->read() ;
  lastEncPos[17] = encoders[8]->read() ;
  // Resync scroll positions to current selections
  chAScroll = chBScroll = lumaSourceScroll = settingsScroll = 0 ;
  keepMenuCursorVisible(chASelection, chAScroll, NUM_SOURCES) ;
  keepMenuCursorVisible(chBSelection, chBScroll, NUM_SOURCES) ;
  keepMenuCursorVisible(lumaSourceCursor, lumaSourceScroll, NUM_LUMA_SOURCES) ;
  keepMenuCursorVisible(settingsCursor, settingsScroll, SETTINGS_COUNT) ;
  patchScroll = audioPatchScroll = postFxScroll = 0 ;
  keepMenuCursorVisible(patchCursor, patchScroll, autoeyez::videoPatchSlotCount) ;
  keepMenuCursorVisible(audioPatchCursor, audioPatchScroll, autoeyez::audioPatchSlotCount) ;
  for(int i = 0 ; i < 8 ; i++) { LED_ON[i] = false ; }
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
    keepMenuCursorVisible(enc7Mode, postFxScroll, ENC7_MODE_COUNT) ;
    enc2Accum = 0 ;
    lastEncPos[2] = encoders[2]->read() ;
    enc5Accum = 0 ;
    lastEncPos[5] = encoders[5]->read() ;
    enc6Accum = 0 ;
    lastEncPos[6] = encoders[6]->read() ;
    for(int i = 0 ; i < 9 ; i++) { labelUpdated[i] = true ; graphUpdated[i] = true ; }
    for(int i = 0 ; i < 4 ; i++) { controlUpdated[i] = true ; functionUpdated[i] = true ; }
  }
  for(int i=0;i<9;++i){long pos=encoders[i]->read();lastEncPos[i]=lastEncPos[i+9]=pos;}
  enc0Accum=enc1Accum=enc2Accum=enc3Accum=enc4Accum=enc5Accum=enc6Accum=enc7Accum=enc8Accum=lineInGainAccum=0;

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
  autoeyez::ControllerLoopSample loopSample(controllerPerf, micros());
  reportControllerPerf();
  checkSerial() ;
  if (videoFpsValid && millis() - videoFpsAt > 3000) {
    videoFpsValid = false ; videoFpsDirty = true ;
  }
  if (videoFpsDirty) { videoFpsDirty = false ; flushDisplay(8) ; }
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
        Serial.println("Boot finalized (R4 safe initialization)") ;
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

    if (shaderOverlayActive) {
      bool localInput = (pressed & mask) != 0 ;
      for (int i = 0 ; i < 9 && !localInput ; ++i)
        localInput = encoders[i]->read() != shaderOverlayEncoderPos[i] ;
      if (localInput || millis() - shaderOverlaySince >= SHADER_OVERLAY_MS)
        dismissShaderOverlay() ;
    }

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
        if (prev != pendingPage) { previewPageDisplay8(pendingPage) ; suppressDisplay8Draw = true ; }
      }
      if (pressed & (1 << BTN_MID)) {
        int prev = pendingPage ; pendingPage = 1 ;
        ringConsumed |= (1 << BTN_MID) ;
        if (prev != pendingPage) { previewPageDisplay8(pendingPage) ; suppressDisplay8Draw = true ; }
      }
      if (pressed & (1 << BTN_HI)) {
        int prev = pendingPage ; pendingPage = 2 ;
        ringConsumed |= (1 << BTN_HI) ;
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
        if (page == 0) {
          beatClock.tap(millis());
        } else if (page == 1) {
          activateClipMenuSelection();
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
          activateClipMenuSelection();
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
            Serial.print("SHADER_BAND:");Serial.println(react_band);

            // Entering reactive mode: hold both graph pages at their bases.
            if (prev_band < 0 && react_band >= 0) {
              for (int j = 0 ; j < 17 ; j++) {
                if (j != 8) saved_function_value[j] = function_value[j] ;
              }
            }
            // Exiting reactive mode: restore both graph pages and resend values.
            if (prev_band >= 0 && react_band < 0) {
              for (int j = 0 ; j < 17 ; j++) {
                if (j == 8) continue ;
                function_value[j] = saved_function_value[j] ;
                if (j < 8) graphUpdated[j] = false ;
                usbMIDI.sendControlChange(cc_encoder[j], function_value[j], 1) ; usbMIDI.send_now() ;
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
            Serial.println(passthruEnabled ? "AUDIO_PASS:ON" : "AUDIO_PASS:OFF") ;
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
        // Source Config: toggle selected enable or enter/leave its value editor.
        if (released & (1 << BTN_1)) {
          int action=sourceConfigMenu.click(); enc0Accum=0;
          if(action==1){
            lumaLowEnabled=!lumaLowEnabled;
            char cmd[24];snprintf(cmd,sizeof(cmd),"LUMA:%d",lumaLowEnabled?lumaKeyValue:0);sendVideoCommand(cmd);
          } else if(action==2){
            lumaHighEnabled=!lumaHighEnabled;
            char cmd[24];snprintf(cmd,sizeof(cmd),"LUMA_HIGH_EN:%d",lumaHighEnabled?1:0);sendVideoCommand(cmd);
          }
          page2Updated[P2_LUMA]=false;
        }
        if (released & (1 << BTN_2)) {
          if(rendererCursor==0)rendererLumaInvert=!rendererLumaInvert;
          else rendererMirror=!rendererMirror;
          usbMIDI.sendControlChange(rendererCursor==0?100:101,
            (rendererCursor==0?rendererLumaInvert:rendererMirror)?127:0,1);usbMIDI.send_now();
          page2Updated[P2_LUMA_HI]=false;
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
            if(!patchLoadPending && !savePatch(patchCursor)){
              patchFlashActive=true;patchFlashSaved=false;patchFlashError=true;patchFlashTimer=millis();page2Updated[P2_PATCH]=false;
            }
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
            if(!patchLoadPending){
              char path[24];snprintf(path,sizeof(path),"/AUDIO%02d.TXT",audioPatchCursor+1);patchSaveHadPrevious=SD.exists(path);
              beginPatchTransaction("ASAVE",audioPatchCursor,nullptr);
              const char marker[]="marker=1\r\n";
              if(recordPendingSave(marker,sizeof(marker)-1)&&saveAudioMarker(audioPatchCursor)){}
              else {patchLoadPending=false;erasePatchJournal();audioPatchFlashActive=true;audioPatchFlashSaved=true;audioPatchFlashError=true;audioPatchFlashTimer=millis();page2Updated[P2_AUDIO]=false;}
            }
          }
        }
        if (released & (1 << BTN_7)) {
          if (!btn7HeldLong) {
            if (autoeyez::validControllerAudioSlot(audioPatchCursor) && audioPatchFilled[audioPatchCursor]) {
              if(!patchLoadPending)beginPatchTransaction("ALOAD",audioPatchCursor,nullptr);
              else if(!queuedRecalls.push(true,audioPatchCursor)){recallQueueFullAt=millis();Serial.println("PATCH_ERROR:RECALL_QUEUE_FULL");page2Updated[P2_AUDIO]=false;}
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

    // Share the newest coherent FFT snapshot; the sender keeps its existing gates.
    refreshControllerFft();
    beatClock.feed(fftSnapshot,millis());
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
            memcpy(saved_function_value, function_reset, sizeof(saved_function_value)) ;
            memset(control, 0, sizeof(control)) ;
            memset(function, 0, sizeof(function)) ;
            react_band = -1 ;
            for (int band = 0 ; band < 3 ; ++band) {
              usbMIDI.sendControlChange(cc_reactive[band], 0, 1) ;
              for (int effect = 0 ; effect < 4 ; ++effect) postFxReactiveValues[band][effect] = 0 ;
              for (int effect = 0 ; effect < 5 ; ++effect) processingReactiveValues[band][effect] = 0 ;
              holdReactiveValues[band] = 0 ;
              peekReactiveValues[band] = 0 ;
            }
            Serial.println("SHADER_BAND:-1") ;
            Serial.println("SHADER_AUDIO_RESET") ;
            codecLevel = codecStatic = 0 ;
            codecLow = 100 ; codecMid = codecHigh = 80 ; codecRate = 64 ;
            sendCodecSettings() ;
            chromaValue = crushValue = pixelValue = 0 ;
            bleedValue = alignValue = ghostValue = wobbleValue = overdriveValue = 0 ;
            holdValue = peekValue = peekMaskModeValue = 0 ;
            chASource = chASelection = 0 ;
            mixValue = 0 ; mixValueSmoothed = 0 ;
            lumaKeyValue = 0 ; lumaHighValue = 127 ;
            lumaLowEnabled = lumaHighEnabled = false ;
            lumaSourceSel = lumaSourceCursor = 0 ;
            sendVideoCommand("CH_A:0") ;
            sendVideoCommand("MIX:0") ;
            sendVideoCommand("LUMA:0") ;
            sendVideoCommand("LUMA_HIGH:127") ;
            sendVideoCommand("LUMA_HIGH_EN:0") ;
            sendVideoCommand("LUMA_SRC:0") ;
            for(int j = 0 ; j < 9 ; j++) { labelUpdated[j] = false ; graphUpdated[j] = false ; }
            for(int j = 0 ; j < 4 ; j++) { controlUpdated[j] = false ; }
            for(int j = 0 ; j < P2_COUNT ; j++) page2Updated[j] = false ;
            usbMIDI.sendControlChange(cc_control[7], 127, 1) ; usbMIDI.send_now() ;
            for(int j = 0 ; j < 7 ; j++) {
              usbMIDI.sendControlChange(cc_control[j], 0, 1) ; usbMIDI.send_now() ;
            }
            for(int cc = 94 ; cc <= 97 ; ++cc) {
              usbMIDI.sendControlChange(cc, 0, 1) ; usbMIDI.send_now() ;
            }
            for(int cc = 102 ; cc <= 107 ; ++cc) {
              usbMIDI.sendControlChange(cc, 0, 1) ; usbMIDI.send_now() ;
            }
            // A changed physical value also clears any Force note override.
            usbMIDI.sendControlChange(108, 1, 1) ; usbMIDI.send_now() ;
            usbMIDI.sendControlChange(108, 0, 1) ; usbMIDI.send_now() ;
            usbMIDI.sendControlChange(109, 0, 1) ; usbMIDI.send_now() ;
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
        int32_t delta = autoeyez::encoderDifference(lastEncPos[temp_function],pos);
        lastEncPos[temp_function] = pos ;
        if (temp_function == 8) {
          // The Encoder library reports four edges per physical detent.
          // Keep the remainder so one detent changes line-in gain by one step.
          lineInGainAccum += delta ;
          delta = autoeyez::takeSteps(lineInGainAccum, 4) ;
          if (!delta) continue ;
          function_value[8] = autoeyez::boundedValue(function_value[8],delta*function_direction[8],0,127);
        } else {
          function_value[temp_function] = autoeyez::boundedValue(function_value[temp_function],int64_t(encoder_speed)*delta*function_direction[temp_function],0,127);
        }
        if (i < 8) { graphUpdated[i] = false ; }
        if (i == 8 && page == 0) { graphUpdated[8] = false ; }
        int sendVal = (temp_function == 8) ? map(function_value[temp_function], 0, 127, 0, 32) : function_value[temp_function] ;
        usbMIDI.sendControlChange(cc_encoder[temp_function], sendVal, 1) ; usbMIDI.send_now() ;
      }
    }

    // ==================== ENCODER 8 — page 1 clip menu ====================
    if (page == 1) {
      long pos8 = encoders[8]->read() ;
      enc8Accum += autoeyez::encoderDifference(lastEncPos[17],pos8) ;
      lastEncPos[17] = pos8 ;
        int delta=autoeyez::takeSteps(enc8Accum,4);
        if(delta){
          clipMenu.step(delta,clipCount);
          videoDisplayUpdated=false;
        }
    }

    // ==================== PAGE 2 ENCODERS ====================
    if (page == 2) {
      // Encoder 0: navigate Source Config; edit the opened threshold.
      {
        long pos=encoders[0]->read();enc0Accum+=autoeyez::encoderDifference(lastEncPos[0],pos);lastEncPos[0]=pos;
        int threshold=sourceConfigMenu.editing?2:4;
        int delta=autoeyez::takeSteps(enc0Accum,threshold);
        if(delta){
          sourceConfigMenu.turn(delta,lumaKeyValue,lumaHighValue);
          if(sourceConfigMenu.editing){
            char cmd[24];
            if(sourceConfigMenu.cursor==1)snprintf(cmd,sizeof(cmd),"LUMA:%d",lumaLowEnabled?lumaKeyValue:0);
            else snprintf(cmd,sizeof(cmd),"LUMA_HIGH:%d",lumaHighValue);
            sendVideoCommand(cmd);
          }
          page2Updated[P2_LUMA]=false;
        }
      }
      // Encoder 1: navigate Renderer rows.
      {
        long pos=encoders[1]->read();enc1Accum+=autoeyez::encoderDifference(lastEncPos[1],pos);lastEncPos[1]=pos;
        int delta=autoeyez::takeSteps(enc1Accum,4);if(delta){rendererCursor=autoeyez::wrappedValue(rendererCursor,delta,2);page2Updated[P2_LUMA_HI]=false;}
      }

      // Encoder 2: scroll through settings — threshold 4
      {
        long pos2 = encoders[2]->read() ;
        enc2Accum += autoeyez::encoderDifference(lastEncPos[2],pos2) ;
        lastEncPos[2] = pos2 ;
        int delta=autoeyez::takeSteps(enc2Accum,4);
        if(delta){
          settingsCursor=autoeyez::wrappedValue(settingsCursor,delta,SETTINGS_COUNT);
          keepMenuCursorVisible(settingsCursor,settingsScroll,SETTINGS_COUNT);
          page2Updated[P2_FADE]=false;
        }
      }

      // Encoder 3: FFT params (Smooth/Attack/Decay/B-Decay) — threshold 2
      {
        long pos3 = encoders[3]->read() ;
        enc3Accum += autoeyez::encoderDifference(lastEncPos[3],pos3) ;
        lastEncPos[3] = pos3 ;
        int delta = 2 * autoeyez::takeSteps(enc3Accum,2);
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
        enc5Accum += autoeyez::encoderDifference(lastEncPos[5],pos5) ;
        lastEncPos[5] = pos5 ;
        int delta=autoeyez::takeSteps(enc5Accum,4);
        if(delta){
          patchCursor=autoeyez::boundedValue(patchCursor,delta,0,autoeyez::videoPatchSlotCount-1);
          keepMenuCursorVisible(patchCursor,patchScroll,autoeyez::videoPatchSlotCount);
          page2Updated[P2_PATCH]=false;
        }
      }

      // Encoder 6: audio patch scroll — threshold 4
      {
        long pos6 = encoders[6]->read() ;
        enc6Accum += autoeyez::encoderDifference(lastEncPos[6],pos6) ;
        lastEncPos[6] = pos6 ;
        int delta=autoeyez::takeSteps(enc6Accum,4);
        if(delta){
          audioPatchCursor=autoeyez::boundedValue(audioPatchCursor,delta,0,autoeyez::audioPatchSlotCount-1);
          keepMenuCursorVisible(audioPatchCursor,audioPatchScroll,autoeyez::audioPatchSlotCount);
          page2Updated[P2_AUDIO]=false;
        }
      }

      // Encoder 7: scroll the post-FX list; edit only after pressing it.
      {
        long pos7 = encoders[7]->read() ;
        enc7Accum += autoeyez::encoderDifference(lastEncPos[7],pos7) ;
        lastEncPos[7] = pos7 ;
        int delta = autoeyez::takeSteps(enc7Accum,postFxEditing ? 2 : 4);
        if (delta != 0) {
          if (!postFxEditing) {
            enc7Mode = autoeyez::boundedValue(enc7Mode,delta,0,ENC7_MODE_COUNT-1) ;
            keepMenuCursorVisible(enc7Mode, postFxScroll, ENC7_MODE_COUNT) ;
          } else {
            switch (enc7Mode) {
              case 0: {
                codecLevel = constrain(codecLevel + delta, 0, 127);
                sendCodecSettings();
                break;
              }
              case 1: {
                codecStatic = constrain(codecStatic + delta, 0, 127);
                sendCodecSettings();
                break;
              }
              case 2: case 3: case 4: {
                int legacyMode = enc7Mode - 1;
                int* base[] = { nullptr, &chromaValue, &crushValue, &pixelValue } ;
                int& value = react_band < 0 ? *base[legacyMode] :
                             postFxReactiveValues[react_band][legacyMode] ;
                value = constrain(value + delta, 0, 127) ;
                usbMIDI.sendControlChange(94 + legacyMode, value, 1) ; usbMIDI.send_now() ;
                break ;
              }
              case 5: case 6: case 7: case 8: case 9: {
                const int effect = enc7Mode - 5 ;
                int* base[] = { &bleedValue, &alignValue, &ghostValue, &wobbleValue, &overdriveValue } ;
                int& value = react_band < 0 ? *base[effect] : processingReactiveValues[react_band][effect] ;
                value = constrain(value + delta, 0, 127) ;
                usbMIDI.sendControlChange(102 + effect, value, 1) ; usbMIDI.send_now() ;
                break ;
              }
              case 10: {
                int& value = react_band < 0 ? holdValue : holdReactiveValues[react_band] ;
                value = constrain(value + delta, 0, 127) ;
                usbMIDI.sendControlChange(107, value, 1) ; usbMIDI.send_now() ;
                break ;
              }
              case 11: {
                int& value = react_band < 0 ? peekValue : peekReactiveValues[react_band] ;
                value = constrain(value + delta, 0, 127) ;
                usbMIDI.sendControlChange(108, value, 1) ; usbMIDI.send_now() ;
                break ;
              }
              case 12: {
                peekMaskModeValue = delta > 0 ? 1 : 0 ;
                usbMIDI.sendControlChange(109, peekMaskModeValue ? 127 : 0, 1) ; usbMIDI.send_now() ;
                break ;
              }
              case 13: case 14: case 15: case 16: {
                int* controls[] = { &codecLow, &codecMid, &codecHigh, &codecRate };
                int& value = *controls[enc7Mode - 13];
                value = constrain(value + delta, 0, 127);
                sendCodecSettings();
                break;
              }
            }
          }
          page2Updated[P2_MOSH] = false ;
        }
      }

      // Encoder 4: luma source — threshold 4
      {
        long pos4 = encoders[4]->read() ;
        enc4Accum += autoeyez::encoderDifference(lastEncPos[4],pos4) ;
        lastEncPos[4] = pos4 ;
        int delta=autoeyez::takeSteps(enc4Accum,4);
        if(delta){
          lumaSourceCursor=autoeyez::boundedValue(lumaSourceCursor,delta,0,MAX_LUMA_SOURCE_INDEX);
          keepMenuCursorVisible(lumaSourceCursor,lumaSourceScroll,NUM_LUMA_SOURCES);
          page2Updated[P2_LUMA_SRC]=false;
        }
      }

      // Encoder 8: mix (threshold 2) or source list (threshold 4)
      {
        long pos8 = encoders[8]->read() ;
        enc8Accum += autoeyez::encoderDifference(lastEncPos[17],pos8) ;
        lastEncPos[17] = pos8 ;
        int threshold = (mixerSubMode == 1) ? 2 : 4 ;
        int delta=autoeyez::takeSteps(enc8Accum,threshold);
        if(delta){
          if(mixerSubMode==1){mixValue=autoeyez::boundedValue(mixValue,delta,0,127);char cmdBuf[16];snprintf(cmdBuf,16,"MIX:%d",mixValue);sendVideoCommand(cmdBuf);}
          else {int& selection=mixerSubMode==0?chASelection:chBSelection;int& scroll=mixerSubMode==0?chAScroll:chBScroll;selection=autoeyez::boundedValue(selection,delta,0,MAX_SOURCE_INDEX);keepMenuCursorVisible(selection,scroll,NUM_SOURCES);}
          page2Updated[P2_MIXER]=false;
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

    for (int i=0;i<4;i++) LED_ON[LED_1+i]=beatClock.pulse(millis()) && beatClock.step==i+1;
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
    usbMIDI.read() ;  // keep USB healthy before display draws
    done = false ;

    if (shaderOverlayActive && shaderOverlayDirty) {
      drawShaderOverlay() ;
      return ;
    }

    if (!done && page == 2) {
      if (!done && !page2Updated[P2_FADE])    { drawSettingsDisplay() ;   done = true ; return ; }
      if (!done && !page2Updated[P2_FFT_SMOOTH]) { drawFftSmoothDisplay() ; done = true ; return ; }
      if (!done && !page2Updated[P2_LUMA])    { drawLumaKeyDisplay() ;    done = true ; return ; }
      if (!done && !page2Updated[P2_LUMA_HI]){ drawLumaHighDisplay() ;   done = true ; return ; }
      if (!done && !page2Updated[P2_LUMA_SRC]) { drawLumaSourceDisplay() ; done = true ; return ; }
      if (!done && !page2Updated[P2_PATCH])   { drawPatchDisplay() ;      done = true ; return ; }
      if (!done && !page2Updated[P2_AUDIO]) { drawAudioPatchDisplay() ; done = true ; return ; }
      if (!done && !page2Updated[P2_MOSH])  { drawMoshDisplay() ;       done = true ; return ; }
      if (!done && !page2Updated[P2_MIXER] && !suppressDisplay8Draw && !shaderOverlayActive) { drawMixerDisplay() ; done = true ; return ; }
      // All page 2 displays now in use (no blank screens)
    }

    // Page 3 owns full-screen displays; legacy arrays cover only pages 1 and 2.
    if (page == 2) return;

    if (!done && page == 1 && !videoDisplayUpdated && !suppressDisplay8Draw && !shaderOverlayActive) { drawVideoDisplay() ; done = true ; return ; }
    if (!done && page == 0 && !graphUpdated[8] && !suppressDisplay8Draw && !shaderOverlayActive)     { updateVolumeDisplay() ; graphUpdated[8] = true ; done = true ; return ; }

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


bool erasePatchJournal(){
  SD.remove("/PTX.TMP");
  if(SD.exists("/PTX.BAK")&&!SD.remove("/PTX.BAK"))return false;
  if(SD.exists("/PTX.TXT")&&!SD.remove("/PTX.TXT"))return false;
  return true;
}
bool recordPendingSave(const void* data,size_t length){
  pendingSave=autoeyez::PendingSave{};
  strncpy(pendingSave.transaction,patchTransactionId,16);strncpy(pendingSave.operation,patchTransactionOperation,5);pendingSave.slot=patchTransactionSlot;pendingSave.hadPrevious=patchSaveHadPrevious;
  pendingSave.newChecksum=autoeyez::patchChecksum(data,length);
  char path[24];snprintf(path,sizeof(path),patchTransactionOperation[0]=='V'?"/PATCH%02d.TXT":"/AUDIO%02d.TXT",patchTransactionSlot+1);
  if(patchSaveHadPrevious){File previous=SD.open(path);if(!previous)return false;uint32_t hash=2166136261u;while(previous.available()){uint8_t byte=previous.read();hash=autoeyez::patchChecksum(&byte,1,hash);}previous.close();pendingSave.previousChecksum=hash;}
  for(size_t i=0;i<autoeyez::patchFieldCount;++i)pendingSave.values[i]=pendingVideoPatch.values[i];pendingSave.seal();
  return autoeyez::saveVerifiedPatch(SD,"/PTX.TXT","/PTX.TMP","/PTX.BAK",reinterpret_cast<const char*>(&pendingSave),sizeof(pendingSave),FILE_WRITE)==autoeyez::SaveResult::Saved;
}
bool restorePendingSave(){
  if(!pendingSave.valid())return false;
  char path[24],backup[24];snprintf(path,sizeof(path),pendingSave.operation[0]=='V'?"/PATCH%02d.TXT":"/AUDIO%02d.TXT",pendingSave.slot+1);snprintf(backup,sizeof(backup),pendingSave.operation[0]=='V'?"/PATCH%02d.BAK":"/AUDIO%02d.BAK",pendingSave.slot+1);
  auto checksum=[](const char* filename,uint32_t& hash){File file=SD.open(filename);if(!file)return false;hash=2166136261u;while(file.available()){uint8_t byte=file.read();hash=autoeyez::patchChecksum(&byte,1,hash);}file.close();return true;};
  uint32_t current=0;bool exists=checksum(path,current);
  if(pendingSave.hadPrevious && exists && current==pendingSave.previousChecksum)return true;
  if(!pendingSave.hadPrevious && !exists)return true;
  if(exists && current!=pendingSave.newChecksum)return false;
  if(!pendingSave.hadPrevious)return !exists||SD.remove(path);
  uint32_t old=0;if(!checksum(backup,old)||old!=pendingSave.previousChecksum)return false;
  if(exists&&!SD.remove(path))return false;
  return SD.rename(backup,path);
}
void recoverPendingSave(){
  if(!autoeyez::recoverPatch(SD,"/PTX.TXT","/PTX.BAK"))return;
  if(!SD.exists("/PTX.TXT"))return;
  File file=SD.open("/PTX.TXT");autoeyez::PendingSave recovered;
  bool okay=file&&file.size()==sizeof(recovered)&&file.read(reinterpret_cast<uint8_t*>(&recovered),sizeof(recovered))==int(sizeof(recovered));file.close();
  if(!okay||!recovered.valid()){Serial.println("SD_ERROR:TRANSACTION_JOURNAL");return;}
  pendingSave=recovered;strncpy(patchTransactionId,recovered.transaction,17);strncpy(patchTransactionOperation,recovered.operation,6);patchTransactionSlot=recovered.slot;patchSaveHadPrevious=recovered.hadPrevious;
  for(size_t i=0;i<autoeyez::patchFieldCount;++i){pendingVideoPatch.values[i]=recovered.values[i];pendingVideoPatch.seen[i]=true;}
  patchLoadPending=true;patchRecoveringSave=true;patchRecoverUndo=false;patchTransactionSent=millis()-501;
  Serial.println("PATCH:RECOVERING_PENDING_SAVE");
}
