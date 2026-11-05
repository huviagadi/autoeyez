#include "fft_band_patch.h"
#include "fft_envelope_runtime.h"
#include "color_fbo.h"
/*
 * Copyright (c) 2013 Dan Wilcox <danomatika@gmail.com>
 *
 * BSD Simplified License.
 * For information on usage and redistribution, and for a DISCLAIMER OF ALL
 * WARRANTIES, see the file, "LICENSE.txt," in this distribution.
 *
 * See https://github.com/danomatika/ofxMidi for documentation
 *
 */

/*
 * =============================================================================
 * autowaaave — GPU Video Feedback Processor
 * Part of the autoeyez video synthesis system
 * =============================================================================
 *
 * Based on auto_waaave v1.5 by Andrei Jay (ex-zee-ex)
 * https://github.com/ex-zee-ex/auto_waaave
 *
 * This openFrameworks application runs on a Raspberry Pi 5 and performs
 * real-time GPU-accelerated video processing with feedback effects.
 *
 * INTEGRATION WITH AUTOEYEZ:
 *   Input:
 *     - RGB shared-memory input assembled from autoclip's raw YUV420p TCP
 *       stream on port 1237 by stream_receive_shm.service
 *     - MIDI CC messages from automidi (Teensy) via USB
 *     - Audio from HiFiBerry line-in for FFT analysis
 *
 *   Output:
 *     - HDMI to display/capture (processed video with effects)
 *
 *   Control flow:
 *     automidi (Teensy)
 *       → USB MIDI
 *       → THIS APP (receives CC messages)
 *       → GPU shader processing
 *       → HDMI output
 *
 * KEY FEATURES:
 *   - Video feedback with UV displacement (warp, zoom, rotate)
 *   - HSB color manipulation (hue shift, saturation, brightness)
 *   - Luma keying for compositing
 *   - Temporal filtering (frame blending, resonance)
 *   - Sharpening post-process
 *   - Audio-reactive modulation (FFT low/mid/high bands)
 *   - P-lock recording (parameter automation at 30fps × 8 seconds)
 *   - 32 audio patches (stored on Pi)
 *
 * MIDI CC MAP (from automidi):
 *   CC 2:    Fade to black
 *   CC 7:    Audio input gain
 *   CC 16:   Luma key threshold
 *   CC 17:   Feedback mix
 *   CC 18:   Hue shift
 *   CC 19:   Saturation
 *   CC 20:   Brightness (page 1) / Value (page 0)
 *   CC 21:   Blur
 *   CC 22:   Bloom
 *   CC 23:   Sharpen
 *   CC 91:   Patch load latch (127=start, 0=end)
 *   CC 92:   Save audio patch (value = slot)
 *   CC 93:   Load audio patch (value = slot)
 *   CC 120-127: Spatial parameters (X/Y offset, zoom, rotate, hue mod/lfo/off)
 *   + many more for toggles and multipliers (see midibiz function)
 *
 * MANUAL CONTROLS:
 *   Live MIDI targets feed existing parameter pickup and smoothing.
 *   No parameter automation timeline is recorded or played.
 *
 * FRAMEBUFFER SYSTEM:
 *   Uses a circular buffer of 60 framebuffers for video delay/feedback.
 *   At 30fps this provides 2 seconds of delay time.
 *   The delay tap position is controlled by MIDI CC 127.
 *
 * =============================================================================
 */

#include "ofApp.h"
#include "patch_latch_timeout.h"
#include "patch_transaction_io.h"
#include "video_patch_schema.h"
#include "audio_patch_schema.h"
#include "render_profile.h"
#include "patch_slot_bounds.h"
#include <chrono>
#include <map>
#include <sstream>
#include <algorithm>
namespace {
using AuditClock = std::chrono::steady_clock;
static ofShader shaderOptimizedMixer, shaderChromaCrush, shaderMoshParams, shaderMoshPrepared;
static ofFbo moshParams;
static int optimizationMode=autoeyez::safeRenderMode;

void refreshRenderProfile() {
    const auto profile = autoeyez::loadRenderProfile(autoeyez::persistentRenderModePath());
    optimizationMode = profile.mode;
    ofLogNotice() << "RENDER_PROFILE mode=" << optimizationMode
                  << " source=" << profile.source
                  << " path=" << autoeyez::persistentRenderModePath();
    if (profile.invalidPersistent || profile.invalidOverride) {
        ofLogWarning() << "RENDER_PROFILE invalid settings ignored; persistent_invalid="
                       << profile.invalidPersistent
                       << " override_invalid=" << profile.invalidOverride;
    }
}
static double auditUpdateMs=0, auditDrawMs=0, auditGpuMs=0;
static unsigned auditFrames=0, auditDraws=0, auditGpuFrames=0, auditInputs=0;
static AuditClock::time_point auditStart=AuditClock::now();
struct RenderPassAudit {
    bool active=(optimizationMode & 8)!=0;
    AuditClock::time_point last;
    static std::map<std::string,double>& totals(){static std::map<std::string,double> v;return v;}
    RenderPassAudit(){if(active){glFinish();last=AuditClock::now();}}
    void mark(const char* name){if(!active)return;glFinish();auto now=AuditClock::now();
        totals()[name]+=std::chrono::duration<double,std::milli>(now-last).count();last=now;}
    ~RenderPassAudit(){if(!active)return;static unsigned frames=0;static auto start=AuditClock::now();
        ++frames;if(std::chrono::duration<double>(AuditClock::now()-start).count()>=5){
            std::ostringstream line;for(auto& item:totals())line<<item.first<<"="<<item.second/frames<<" ";
            ofLogNotice()<<"PASS_PERF synchronized_ms "<<line.str();totals().clear();frames=0;start=AuditClock::now();}}
};
struct AuditUpdate {
    AuditClock::time_point start=AuditClock::now();
    ~AuditUpdate() {
        auditUpdateMs+=std::chrono::duration<double,std::milli>(AuditClock::now()-start).count();
        ++auditFrames;
        double seconds=std::chrono::duration<double>(AuditClock::now()-auditStart).count();
        if(seconds>=5) {
            ofLogNotice()<<"STAGE_PERF fps="<<auditFrames/seconds
                <<" input_fps="<<auditInputs/seconds
                <<" update_ms="<<auditUpdateMs/auditFrames
                <<" draw_cpu_ms="<<(auditDraws ? auditDrawMs/auditDraws : 0)
                <<" draw_gpu_ms="<<(auditGpuFrames ? auditGpuMs/auditGpuFrames : -1);
            auditFrames=auditDraws=auditGpuFrames=auditInputs=0;
            auditUpdateMs=auditDrawMs=auditGpuMs=0;
            auditStart=AuditClock::now();
        }
    }
};
struct AuditDraw {
    AuditClock::time_point start=AuditClock::now();
    bool active=false;
    AuditDraw() {
        static GLuint queries[8]={};
        static bool pending[8]={};
        static unsigned cursor=0;
        if(GLEW_ARB_timer_query) {
            if(queries[0]==0) glGenQueries(8,queries);
            unsigned i=cursor++%8;
            GLint available=0;
            if(pending[i]) glGetQueryObjectiv(queries[i],GL_QUERY_RESULT_AVAILABLE,&available);
            if(pending[i] && available) {
                GLuint64 ns=0;
                glGetQueryObjectui64v(queries[i],GL_QUERY_RESULT,&ns);
                auditGpuMs+=ns/1000000.0; ++auditGpuFrames;
                pending[i]=false;
            }
            if(!pending[i]) {
                glBeginQuery(GL_TIME_ELAPSED,queries[i]);
                pending[i]=true; active=true;
            }
        }
    }
    ~AuditDraw() {
        if(active) glEndQuery(GL_TIME_ELAPSED);
        auditDrawMs+=std::chrono::duration<double,std::milli>(AuditClock::now()-start).count();
        ++auditDraws;
    }
};
}

#include <algorithm>
#include <cmath>
#include <fstream>

#include "iostream"
#include <sys/stat.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

// =============================================================================
// CONSTANTS
// =============================================================================

// MIDI center value for bipolar parameters (0-127 range, center at 63.5)
#define MIDI_MAGIC 63.50f

// Threshold for MIDI control "pick-up" (prevents jumps when knob doesn't match)
#define CONTROL_THRESHOLD .035f

// =============================================================================
// CONFIGURATION FLAGS
// =============================================================================

// Input remains 720×480; all feedback FBOs process at 1080p.
bool scaleswitch=1;

// 30 RGBA frames at 1080p need roughly 237 MiB. At the 30 fps render target,
// this provides one second of feedback history and fits the Pi 5 CMA budget.
const int framebufferLength=30;

// CC 21 (Blur) blends a recent frame. Keep this independent from the much
// longer feedback-delay ring, whose frame selection is controlled elsewhere.
const int temporalFilterDelayFrames=1;

// =============================================================================
// MIDI PATCH LOADING STATE
// =============================================================================

// When loading a patch, forceMidiWrite bypasses the pick-up threshold
// so all parameter values are immediately applied
bool forceMidiWrite = false ;
unsigned long forceMidiWriteTimer = 0 ;
unsigned long midiReconnectTimer = 0 ;

// =============================================================================
// FADE TO BLACK
// =============================================================================

// Blackout overlay controlled by CC 2 (for smooth fade transitions)
float blackoutAmount = 0 ;
float blackoutSmoothed = 0 ;

// =============================================================================
// INPUT AND OUTPUT MODES
// =============================================================================

// Input source: 0 = Pi camera, 1 = USB/network video input
// For autoeyez, always 1 (video comes from autoclip via TCP)
bool inputswitch=1;

// Feedback mode:
//   0 = Wet (internal feedback): output feeds back into itself
//   1 = Dry (delay mode): input feeds delay buffer, no self-feedback
bool wet_dry_switch=1;

// Aspect ratio handling:
//   0 = SD 4:3 aspect (for composite/480i sources)
//   1 = HD 16:9 corner crop (for HD sources)
int hdmi_aspect_ratio_switch=0;

// =============================================================================
// SHADER PARAMETERS (base values from keyboard/direct control)
// =============================================================================
// These single-letter variables are legacy from auto_waaave keyboard mapping.
// They're combined with p-lock and audio-reactive values in parametersAssign().

float az = 1.0;   // Z displacement (zoom)
float sx = 0;     // X displacement
float dc = 0;     // Y displacement (d/c keys)
float fv = 1;     // Hue attenuation
float gb = 1;     // Saturation attenuation
float hn = 1;     // Brightness attenuation
float jm = 0.0;   // Feedback mix amount
float kk = 0.0;   // Luma key threshold
float ll = 0.0;   // Sharpen amount
float pixelAmount = 0.0; // Pixelize post-FX (Teensy CC 97)
float chromaAmount = 0.0; // Chromatic aberration (Teensy CC 95)
float crushAmount = 0.0;  // Color depth reduction (Teensy CC 96)
float moshAmount = 0.0;   // Motion-vector datamosh (Teensy CC 94)
float pixelBase = 0.0, chromaBase = 0.0, crushBase = 0.0, moshBase = 0.0;
float holdBase = 0.0, holdDepth[3] = {}, holdAmount = 0.0;
int holdFrameSlots[3] = {-1, -1, -1};
unsigned int holdNextRefresh[3] = {};
// Input processing follows autoclip's composite and precedes the feedback mixer.
// Indices: chroma bleed, chroma alignment, ghost, time-base wobble, overdrive.
float inputProcessingBase[5] = {};
float inputProcessingDepth[3][5] = {};
float inputProcessingAmount[5] = {};
float qw = 0.0;   // Rotation
float er = 1.0;   // Hue modulo
float ty = 0.0;   // Hue offset
float ui = 0.0;   // Hue LFO
float op = 0.0;   // Temporal filter mix

float fb1_brightx=0.0;
bool toroidSwitch=0;
bool y_skew_switch=false;
bool x_skew_switch=false;
bool lumakeyInvertSwitch=false;
bool x_mirrorSwitch=false;
bool y_mirrorSwitch=false;
float y_skew=0;
float x_skew=0;
bool mirrorSwitch=false;

int fb0_delayamount=1;  // Minimum 1 to prevent read/write index collision on Mesa

//dummy variables for midi control

const int inputWidth=720;
const int inputHeight=480;
int width=1920;
int height=1080;
constexpr int MOSH_FRAME_WIDTH = 480;
constexpr int MOSH_FRAME_HEIGHT = 270;
constexpr int MOSH_FIELD_WIDTH = 120;
constexpr int MOSH_FIELD_HEIGHT = 68;


// =============================================================================
// AUDIO-REACTIVE MODULATION
// =============================================================================
// Each parameter can be modulated by FFT frequency bands (low/mid/high).
// The automidi controller selects which band to assign via ring buttons.
// lowCn, midCn, highCn are the modulation amounts for parameter n.

// Audio reactive mode: 0=direct MIDI, 1=low band, 2=mid band, 3=high band
int audioReactiveControlSwitch=0;

// Low-frequency (bass) modulation amounts for each parameter
float lowC1=0.0;
float lowC2=0.0;
float lowC3=0.0;
float lowC4=0.0;
float lowC5=0.0;
float lowC6=0.0;
float lowC7=0.0;
float lowC8=0.0;
float lowC9=0.0;
float lowC10=0.0;
float lowC11=0.0;
float lowC12=0.0;
float lowC13=0.0;
float lowC14=0.0;
float lowC15=0.0;
float lowC16=0.0;
float lowC17=0.0, lowC18=0.0, lowC19=0.0, lowC20=0.0;

float midC1=0.0;
float midC2=0.0;
float midC3=0.0;
float midC4=0.0;
float midC5=0.0;
float midC6=0.0;
float midC7=0.0;
float midC8=0.0;
float midC9=0.0;
float midC10=0.0;
float midC11=0.0;
float midC12=0.0;
float midC13=0.0;
float midC14=0.0;
float midC15=0.0;
float midC16=0.0;
float midC17=0.0, midC18=0.0, midC19=0.0, midC20=0.0;

float highC1=0.0;
float highC2=0.0;
float highC3=0.0;
float highC4=0.0;
float highC5=0.0;
float highC6=0.0;
float highC7=0.0;
float highC8=0.0;
float highC9=0.0;
float highC10=0.0;
float highC11=0.0;
float highC12=0.0;
float highC13=0.0;
float highC14=0.0;
float highC15=0.0;
float highC16=0.0;
float highC17=0.0, highC18=0.0, highC19=0.0, highC20=0.0;


bool clear_switch=0;
//toggle switch variables
bool hueInvert=false;
bool saturationInvert=false;
bool brightInvert=false;

bool cam1_h_invert=false;
bool cam1_s_invert=false;
bool cam1_b_invert=false;

bool horizontalMirror=false;
bool verticalMirror=false;

bool clear_flip=false;

bool x_2=false;
bool x_5=false;
bool x_10=false;

bool y_2=false;
bool y_5=false;
bool y_10=false;

bool z_2=false;
bool z_5=false;
bool z_10=false;

bool r_2=false;
bool r_5=false;
bool r_10=false;

bool huexx_0=false;
bool huexx_1=false;
bool huexx_2=false;

bool huexy_0=false;
bool huexy_1=false;
bool huexy_2=false;

bool huexz_0=false;
bool huexz_1=false;
bool huexz_2=false;

//framebuffer management biziness

//framebufferLength is the total number of frames that will be stored.  setting framebufferLength to
//30 while fps is set to 30 means 1 second of video will be stored in memory  
//75 seems to work ok with usb cams but the capture device might need a little more memory

int framedelayoffset=0;  // this is used as an index to the circular framebuffers eeettt
unsigned int framecount=0; // framecounter used to calc offset eeettt

//this is an array of framebuffer objects
ofFbo pastFrames[framebufferLength];


void incIndex()  // call this every frame to calc the offset eeettt
{
    framecount++;
    framedelayoffset=framecount % framebufferLength;
}

// =============================================================================
// MANUAL CONTROL TARGETS
// =============================================================================
// One live target per parameter; the old stationary automation timeline was
// unused. Retain the spare seventeenth lane for existing latch/reset bounds.
const int manualControlCount=17;
float manualControlTargets[manualControlCount]={};
float manualControlSmoothed[manualControlCount]={};
float manualControlSmoothing=.25f;
// Smooth only manual spatial controls; audio modulation remains immediate.
float transform_control_smooth=.75f;
// Used only by the inactive legacy MIDI map. The active map always accepts
// eligible manual writes through its existing pickup/assignment conditions.
bool legacyManualControlsEnabled=true;

float my_normalize=0;

float low_value_smoothed=0.0;
float mid_value_smoothed=0.0;
float high_value_smoothed=0.0;
// Fast attack makes transients visible on the next rendered frame. Decay
// remains eased to avoid flicker after the transient.
autoeyez::EnvelopeRuntime fftEnvelopeRuntime;

ofFbo aspect_fix_fbo;
ofFbo dry_framebuffer;

bool videoReactiveSwitch=false;

float vLumakeyValue=0.0;
float vMix=0.0;
float vHue=0.0;
float vSaturation=0.0;
float vBright=0.0;
float vTemporalFilterMix=0.0;
float vTemporalFilterResonance=0.0;
float vSharpenAmount=0.0;
float vX=0.0;
float vY=0.0;
float vZ=0.0;
float vRotate=0.0;
float vHuexMod=0.0;
float vHuexOff=0.0;
float vHuexLfo=0.0;

//latching
bool midiActiveFloat[17];
bool vmidiActiveFloat[17];
bool midiLowActiveFloat[17];
bool midiMidActiveFloat[17];
bool midiHighActiveFloat[17];

float range=200;
float fftLow;
float fftMid;
float fftHigh;

float c_lumakey_value=1.01;
float c_mix=2;
float c_hue=.25;
float c_sat=.2;
float c_bright=.2;
float c_temporalFilterMix=1.1;
float c_sharpenAmount=.5;
float c_x=.01;
float c_y=.01;
float c_z=.05;
float c_rotate=.314159265;
float c_huex_off=.25;
float c_huex_lfo=.25;

int d_delay;
float d_lumakey_value;
float d_mix;
float d_hue;
float d_sat;
float d_bright;
float d_temporalFilterMix;
float d_temporalFilterResonance;
float d_sharpenAmount;
float d_x;
float d_y;
float d_z;
float d_rotate;
float d_huex_mod;
float d_huex_off;
float d_huex_lfo;
// =============================================================================
// SETUP — Application initialization
// =============================================================================
void ofApp::setup() {
    // Select the approved path before initializing or drawing any video frame.
    refreshRenderProfile();
    forceCcSetup();
    ofDisableArbTex();  // MUST be first - required for Pi 5 GL 3.1

    // Pi 5 clip-space quad for shader rendering
    clipSpaceQuad.setMode(OF_PRIMITIVE_TRIANGLE_STRIP);
    clipSpaceQuad.addVertex(ofVec3f(-1, -1, 0));
    clipSpaceQuad.addVertex(ofVec3f(1, -1, 0));
    clipSpaceQuad.addVertex(ofVec3f(-1, 1, 0));
    clipSpaceQuad.addVertex(ofVec3f(1, 1, 0));
    // The 1080p feedback pipeline sustains a stable 30 fps on Pi 5.
    ofSetFrameRate(30);
    // GLFW/X11 vblank pacing measured ~18 fps on this Pi; retain timer-based
    // 60 fps pacing instead and present the newest completed frame.
    ofSetVerticalSync(false);
    ofBackground(0);
    ofHideCursor();  // No cursor for video output display

    if(!houseCardImage.load("HOUSE_CARD.png")) {
        ofLogError() << "SHOW_OVERLAY failed to load HOUSE_CARD.png";
    }
    if(!houseLogoImage.load("HOUSE_LOGO.png")) {
        ofLogError() << "SHOW_OVERLAY failed to load HOUSE_LOGO.png";
    }
    if(!mainLogoImage.load("ANNE.png")) {
        ofLogError() << "SHOW_OVERLAY failed to load ANNE.png";
    }

    // Initialize subsystems
    // inputSetup() deferred until video socket ready (see update())
    midiSetup();            // USB MIDI from automidi Teensy
    fbDeclareAndAllocate(); // Framebuffer objects for delay/feedback

    // Readiness requires successfully linked shaders and an actual video frame.
    videoShadersReady = loadVideoShaders();
    nextShaderRetryMs = ofGetElapsedTimeMillis() + 4000;
    GLint samples=0;glGetIntegerv(GL_SAMPLES,&samples);
    ofLogNotice()<<"WINDOW_SAMPLES "<<samples;

    // FFT audio analysis (HiFiBerry line-in)
    fft.setup();
    fft.setNormalize(false);

    // Clear p-lock buffers and MIDI state
    clearManualControls();
    midiLatchClear();
}
//-------------------------------------------------------------
void ofApp::midiLatchClear(){
    for(int i=0;i<17;i++){
        vmidiActiveFloat[i]=0;
        midiActiveFloat[i]=0;
        midiLowActiveFloat[i]=0;
        midiMidActiveFloat[i]=0;
        midiHighActiveFloat[i]=0;
    }
}
//--------------------------------------------------------------
void ofApp::update() {
    AuditUpdate audit;
    forceCcPoll();
    pollLumaTrackState();
    pollShowOverlayState();

    if(ofGetElapsedTimeMillis() - midiReconnectTimer > 5000) {
        midiReconnectTimer = ofGetElapsedTimeMillis();
        const auto ports = midiIn.getInPortList();
        const bool connectedPortPresent = midiIn.isOpen()
            && std::find(ports.begin(), ports.end(), midiIn.getName()) != ports.end();
        std::string serialGeneration;
        int controllerReady = 1; // Compatibility when running the older bridge.
        std::ifstream generationFile("/dev/shm/autoeyez_controller_generation");
        if(generationFile.is_open()) {
            controllerReady = 0;
            generationFile >> serialGeneration >> controllerReady;
        }
        const bool generationChanged = !serialGeneration.empty()
                                       && serialGeneration != midiSerialGeneration;
        // The bridge can reopen USB serial without disturbing USB MIDI.
        // Re-subscribe only when the MIDI port vanished or its heartbeat has
        // also stopped; otherwise a serial timeout would interrupt FFT MIDI.
        const unsigned long lastMidi = midiLastMessageMs.load();
        const bool midiStale = lastMidi == 0 ||
            ofGetElapsedTimeMillis() - lastMidi > 5000;
        if(!connectedPortPresent || (generationChanged && midiStale)) {
            midiIn.closePort();
            midiIn.removeListener(this);
            if(controllerReady == 1 && openPreferredMidiPort()) {
                midiIn.ignoreTypes(false, false, false);
                midiIn.addListener(this);
            }
        }
        if(!serialGeneration.empty()) midiSerialGeneration = serialGeneration;
    }

    updateVideoInput();

    // Loading animation
    if(!videoReady) {
        unsigned long now = ofGetElapsedTimeMillis();
        if(now - lastDotUpdate > 400) {
            loadingDotState = (loadingDotState + 1) % 4;
            lastDotUpdate = now;
        }
    }

    publishAppStatus();

    fft.update();
    expirePatchWriteLatch(ofGetElapsedTimeMillis());
    midibiz();
    static unsigned long lastMidiReceiveReport = 0;
    static std::uint64_t previousReceivedFrames = 0;
    const unsigned long midiReportNow = ofGetElapsedTimeMillis();
    if(midiReportNow - lastMidiReceiveReport >= 5000) {
        std::lock_guard<std::mutex> lock(midiFftMutex);
        const auto age = midiFftFrame.valid ?
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - midiFftAt).count() : -1;
        ofLogNotice() << "FFT_MIDI_RX frames_5s="
                      << midiFftFrame.frames - previousReceivedFrames
                      << " age_ms=" << age;
        previousReceivedFrames = midiFftFrame.frames;
        lastMidiReceiveReport = midiReportNow;
    }
    patchTransactionPoll();
    updateManualControls();
    
    if(fft.getLowVal()>my_normalize){
        my_normalize=fft.getLowVal();
    }

}

//--------------------------------------------------------------
void ofApp::draw() {
#ifdef AUTOEYEZ_D3_TRACE_CANDIDATE
    TraceRenderScope traceScope(videoReady?traceInput:nullptr);
#endif
    RenderPassAudit passAudit;
    hdmi_aspect_ratio_switch = 0;  // TEMP: forced for initial Pi 5 test

    // Show loading screen until video is ready
    if(!videoReady) {
        drawLoading();
        return;
    }

    // Opt-in end-to-end clip timing probe. The marker is stamped into the
    // decoded clip pixels on autoclip, so this fires only when that frame has
    // reached the renderer's draw loop. Enable on both Pis for a test run.
    static const bool clipLatencyMarker = [] {
        const char* value = getenv("AUTOEYEZ_CLIP_LATENCY_MARKER");
        return value && std::string(value) == "1";
    }();
    if(clipLatencyMarker) {
        static int lastCode = -1;
        const ofPixels& pixels = cam1.getPixels();
        if(pixels.isAllocated() && pixels.getWidth() >= 144 && pixels.getHeight() >= 12) {
            auto bitAt = [&](int index) {
                const ofColor color = pixels.getColor(index * 12 + 6, 5);
                return (int(color.r) + int(color.g) + int(color.b)) > 384;
            };
            if(bitAt(0) && !bitAt(1) && bitAt(2) && bitAt(3)) {
                int code = 0;
                for(int index = 4; index < 12; ++index)
                    code = (code << 1) | int(bitAt(index));
                if(code != lastCode) {
                    const auto wallNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    ofLogNotice() << "CLIP_LATENCY stage=renderer_draw code=" << code
                                  << " wall_ns=" << wallNs;
                    lastCode = code;
                }
            } else {
                lastCode = -1;
            }
        }
    }

    //the range changes depending on what yr using for sound input.  just usb cams have shitter ranges
    //so 50 works
    //float range=200;

    AuditDraw audit;
    fftAssignValues();
    
    parametersAssign();

    // A zero base value is a true bypass. Once an effect is enabled by its
    // MIDI knob, assigned FFT bands can increase its strength on live audio.
    auto postFxAmount = [&](float base, float low, float mid, float high) {
        if(base <= 0.0001f) return 0.0f;
        float audio = low * low_value_smoothed +
                      mid * mid_value_smoothed +
                      high * high_value_smoothed;
        return ofClamp(base + 3.0f * audio, 0.0f, 1.0f);
    };
    moshAmount = ofClamp(postFxAmount(moshBase, lowC17, midC17, highC17) + forceCcOffset[94], 0.0f, 1.0f);
    chromaAmount = ofClamp(postFxAmount(chromaBase, lowC18, midC18, highC18) + forceCcOffset[95], 0.0f, 1.0f);
    crushAmount = ofClamp(postFxAmount(crushBase, lowC19, midC19, highC19) + forceCcOffset[96], 0.0f, 1.0f);
    pixelAmount = ofClamp(postFxAmount(pixelBase, lowC20, midC20, highC20) + forceCcOffset[97], 0.0f, 1.0f);
    holdAmount = ofClamp(postFxAmount(holdBase, holdDepth[0], holdDepth[1], holdDepth[2]) + forceCcOffset[107], 0.0f, 1.0f);
    for(int i=0;i<5;++i)
        inputProcessingAmount[i] = postFxAmount(inputProcessingBase[i],
            inputProcessingDepth[0][i], inputProcessingDepth[1][i], inputProcessingDepth[2][i]);
    static unsigned long lastFxAudioLog = 0;
    unsigned long fxNow = ofGetElapsedTimeMillis();
    float fxAssignments = lowC17+lowC18+lowC19+lowC20+
                          midC17+midC18+midC19+midC20+
                          highC17+highC18+highC19+highC20;
    if(fxNow-lastFxAudioLog >= 5000) {
        refreshRenderProfile();
        ofLogNotice() << "RENDER_MODE mode=" << optimizationMode
                      << " blend=" << static_cast<int>(glIsEnabled(GL_BLEND))
                      << " sharpen=" << d_sharpenAmount
                      << " temporal=" << d_temporalFilterMix;
        ofLogNotice() << "FX_AUDIO mosh=" << moshAmount
                      << " chroma=" << chromaAmount
                      << " crush=" << crushAmount
                      << " pixel=" << pixelAmount;
        lastFxAudioLog = fxNow;
    }
    // The slot about to be written contains the oldest history frame. Moving
    // forward through the circular buffer selects recent frames: +1 is the
    // prior frame, so +1 gives Blur the closest safe temporal reference.
    const int temporalFilterIndex =
        (abs(framebufferLength-framedelayoffset)-1 + temporalFilterDelayFrames)
        % framebufferLength;
    
    ofShader& activeMixer=(optimizationMode & 2) ? shaderOptimizedMixer : shader_mixer;
    ofTexture* mixerInput = &cam1.getTexture();
    bool processInput = false;
    for(float amount : inputProcessingAmount) processInput = processInput || amount > 0.0001f;
    if(processInput && shaderInputProcessing.isLoaded()) {
        const int next = 1 - inputProcessingIndex;
        inputProcessingFramebuffer[next].begin();
        shaderInputProcessing.begin();
        shaderInputProcessing.setUniformTexture("tex0", cam1.getTexture(), 0);
        shaderInputProcessing.setUniformTexture("history", inputProcessingFramebuffer[inputProcessingIndex].getTexture(), 1);
        shaderInputProcessing.setUniform2f("texelStep", 1.0f / inputWidth, 1.0f / inputHeight);
        shaderInputProcessing.setUniform1f("time", ofGetElapsedTimef());
        shaderInputProcessing.setUniform1f("bleed", inputProcessingAmount[0]);
        shaderInputProcessing.setUniform1f("align", inputProcessingAmount[1]);
        shaderInputProcessing.setUniform1f("ghost", inputProcessingHistoryValid ? inputProcessingAmount[2] : 0.0f);
        shaderInputProcessing.setUniform1f("wobble", inputProcessingAmount[3]);
        shaderInputProcessing.setUniform1f("overdrive", inputProcessingAmount[4]);
        clipSpaceQuad.draw();
        shaderInputProcessing.end();
        inputProcessingFramebuffer[next].end();
        cam1.getTexture().unbind(0);
        inputProcessingFramebuffer[inputProcessingIndex].getTexture().unbind(1);
        inputProcessingIndex = next;
        inputProcessingHistoryValid = true;
        mixerInput = &inputProcessingFramebuffer[next].getTexture();
    } else if(!processInput) {
        inputProcessingHistoryValid = false;
    }
    framebuffer0.begin();
    // DEBUG: print texture dimensions once
    static bool printed = false;
    if(!printed && cam1.isInitialized()) {
        ofLogNotice() << "TEX_DEBUG cam1=" << cam1.getTexture().getWidth() << "x" << cam1.getTexture().getHeight()
                      << " fb0=" << framebuffer0.getWidth() << "x" << framebuffer0.getHeight();
        printed = true;
    }
    activeMixer.begin();
    
    

    // TODO(scaleswitch): clip-space quad always fills the target, so the
    // 320x240 and other sized draw branches from the 3B+ version no longer
    // apply. Scaling must move to UV math or FBO sizing. Full-size path only.

    // Pi 5: ALL uniforms MUST be set BEFORE draw (shader executes during draw)
    // Bind tex0 (video input) to texture unit 0
    activeMixer.setUniformTexture("tex0", *mixerInput, 0);
    /*
     //textures
     activeMixer.setUniformTexture("fb0", pastFrames[d_delay].getTexture(),1);
     
     if(wet_dry_switch==1){
        //framebuffer0.draw(0,0);
     activeMixer.setUniformTexture("fb1", pastFrames[(abs(framedelayoffset-framebufferLength)-1)%framebufferLength].getTexture(),2);
     }
     if(wet_dry_switch==0){
     activeMixer.setUniformTexture("fb1", dry_framebuffer.getTexture(),2);
     }
     //continuous variables
     activeMixer.setUniform1f("fb0_lumakey_value",d_lumakey_value);
     activeMixer.setUniform1f("fb0_mix",d_mix);
     activeMixer.setUniform3f("fb0_hsbx",ofVec3f(d_hue,d_sat,d_bright));
     activeMixer.setUniform1f("temporalFilterMix",d_temporalFilterMix);
     activeMixer.setUniform1f("fb1_brightx",d_temporalFilterResonance );
     activeMixer.setUniform1f("cam1_brightx",d_cam1_x);
     activeMixer.setUniform1f("fb0_xdisplace",d_x);
     activeMixer.setUniform1f("fb0_ydisplace",d_y);
     activeMixer.setUniform1f("fb0_zdisplace",d_z);
     activeMixer.setUniform1f("fb0_rotate",d_rotate);
     activeMixer.setUniform3f("fb0_huex",ofVec3f(d_huex_mod,d_huex_off,d_huex_lfo));
     
     if(wet_dry_switch==1){
        //framebuffer0.draw(0,0);
     activeMixer.setUniform1i("toroidSwitch",toroidSwitch);
     activeMixer.setUniform1i("mirrorSwitch",0);
     }
     
     if(wet_dry_switch==0){
     activeMixer.setUniform1i("toroidSwitch",0);
     activeMixer.setUniform1i("mirrorSwitch",toroidSwitch);
     }
     
     
     activeMixer.setUniform1i("brightInvert",brightInvert);
     activeMixer.setUniform1i("hueInvert",hueInvert);
     activeMixer.setUniform1i("saturationInvert",saturationInvert);
     
     activeMixer.setUniform1i("horizontalMirror",horizontalMirror);
     activeMixer.setUniform1i("verticalMirror",verticalMirror);
     
     
     activeMixer.setUniform1i("lumakeyInvertSwitch",lumakeyInvertSwitch);
     */
    // Bind feedback textures through openFrameworks. This selects the texture
    // unit and sampler uniform together, and is the supported Mesa path.
    activeMixer.setUniformTexture("fb", pastFrames[d_delay].getTexture(), 1);
    if(wet_dry_switch==1){
        activeMixer.setUniformTexture("temporalFilter",
            pastFrames[temporalFilterIndex].getTexture(), 2);
    }
    if(wet_dry_switch==0){
        activeMixer.setUniformTexture("temporalFilter", dry_framebuffer.getTexture(), 2);
    }
    //send the continuous variables
    activeMixer.setUniform1f("lumakey",d_lumakey_value);
    activeMixer.setUniform1f("fbMix",d_mix);
    activeMixer.setUniform1f("fbHue",d_hue);
    activeMixer.setUniform1f("fbSaturation",d_sat);
    activeMixer.setUniform1f("fbBright",d_bright);
    activeMixer.setUniform1f("temporalFilterMix",d_temporalFilterMix);
    activeMixer.setUniform1f("temporalFilterResonance",d_temporalFilterResonance );
    activeMixer.setUniform1f("fbXDisplace",d_x);
    activeMixer.setUniform1f("fbYDisplace",d_y);
    activeMixer.setUniform1f("fbZDisplace",d_z);
    activeMixer.setUniform1f("fbRotate",d_rotate);
    activeMixer.setUniform3f("fb_huex",ofVec3f(d_huex_mod,d_huex_off,d_huex_lfo));
    activeMixer.setUniform1f("fbHuexMod",d_huex_mod);
    activeMixer.setUniform1f("fbHuexOff",d_huex_off);
    activeMixer.setUniform1f("fbHuexLfo",d_huex_lfo);
    
    //send the switches
    activeMixer.setUniform1i("toroidSwitch",toroidSwitch);
    activeMixer.setUniform1i("mirrorSwitch",mirrorSwitch);
    activeMixer.setUniform1i("brightInvert",brightInvert);
    activeMixer.setUniform1i("hueInvert",hueInvert);
    activeMixer.setUniform1i("saturationInvert",saturationInvert);
    activeMixer.setUniform1i("horizontalMirror",horizontalMirror);
    activeMixer.setUniform1i("verticalMirror",verticalMirror);
    activeMixer.setUniform1i("lumakeyInvertSwitch",lumakeyInvertSwitch);
    
    //send the videoreactive controls
    activeMixer.setUniform1f("vLumakey",vLumakeyValue*c_lumakey_value);
    activeMixer.setUniform1f("vMix",vMix*c_mix);
    activeMixer.setUniform1f("vHue",2.0f*vHue*c_hue);
    activeMixer.setUniform1f("vSat",2.0f*vSaturation*c_sat);
    activeMixer.setUniform1f("vBright",2.0f*vBright*c_bright);
    activeMixer.setUniform1f("vtemporalFilterMix",vTemporalFilterMix*c_temporalFilterMix);
    activeMixer.setUniform1f("vFb1X",vTemporalFilterResonance);
    activeMixer.setUniform1f("vX",.01*vX);
    activeMixer.setUniform1f("vY",.01*vY);
    activeMixer.setUniform1f("vZ",vZ*c_z);
    activeMixer.setUniform1f("vRotate",vRotate*c_rotate);
    activeMixer.setUniform1f("vHuexMod",1.0f-vHuexMod);
    activeMixer.setUniform1f("vHuexOff",vHuexOff*c_huex_off);
    activeMixer.setUniform1f("vHuexLfo",vHuexLfo*c_huex_lfo);

    // Pi 5: Draw clip-space quad (fills entire render target)
    clipSpaceQuad.draw();
    activeMixer.end();
    framebuffer0.end();
    passAudit.mark("mixer");

    // The feedback targets are sampled on texture units 1 and 2 above. Release
    // those bindings before one of the same textures is attached as the next
    // render target. Mesa is less forgiving of this than the legacy Pi stack.
    pastFrames[d_delay].getTexture().unbind(1);
    if(wet_dry_switch==1){
        pastFrames[temporalFilterIndex].getTexture().unbind(2);
    } else {
        dry_framebuffer.getTexture().unbind(2);
    }
    
    // Bypass the post-process at zero. The sharpen controls are unipolar, so
    // either base or video-reactive amount makes the pass active.
    bool doSharpen = d_sharpenAmount > 0.0001f || vSharpenAmount > 0.0001f;
    ofFbo& outputFbo = doSharpen ? sharpenFramebuffer : framebuffer0;

    if(doSharpen) {
        sharpenFramebuffer.begin();
        shaderSharpen.begin();
        // Pi 5 shaders use clip-space geometry. Bind uniforms before drawing:
        // framebuffer0.draw() uses pixel-space geometry and executes too soon.
        shaderSharpen.setUniformTexture("tex0", framebuffer0.getTexture(), 0);
        shaderSharpen.setUniform1f("sharpenAmount",d_sharpenAmount);
        shaderSharpen.setUniform1f("vSharpenAmount",vSharpenAmount*c_sharpenAmount);
        shaderSharpen.setUniform2f("texelStep", 1.0f / width, 1.0f / height);
        clipSpaceQuad.draw();
        shaderSharpen.end();
        sharpenFramebuffer.end();
        framebuffer0.getTexture().unbind(0);
    }

    passAudit.mark("sharpen");
    ofFbo* postSource = &outputFbo;
    if(moshAmount > 0.0001f) {
        // Downsample the live composite. Two small frame buffers let the
        // motion pass compare consecutive frames without a full-size copy.
        int nextFrame = 1 - moshFrameIndex;
        moshFrame[nextFrame].begin();
        shaderPassthrough.begin();
        shaderPassthrough.setUniformTexture("tex", outputFbo.getTexture(), 0);
        clipSpaceQuad.draw();
        shaderPassthrough.end();
        moshFrame[nextFrame].end();
        outputFbo.getTexture().unbind(0);

        if(!moshPrimed) {
            // Start with a clean current image when CC 94 first rises.
            moshAccum[0].begin();
            shaderPassthrough.begin();
            shaderPassthrough.setUniformTexture("tex", outputFbo.getTexture(), 0);
            clipSpaceQuad.draw();
            shaderPassthrough.end();
            moshAccum[0].end();
            outputFbo.getTexture().unbind(0);
            moshAccumIndex = 0;
            moshFrameCount = 0;
            moshPrimed = true;
        } else {
            moshMotion.begin();
            shaderMoshMotion.begin();
            shaderMoshMotion.setUniformTexture("texCurrent", moshFrame[nextFrame].getTexture(), 0);
            shaderMoshMotion.setUniformTexture("texPrevious", moshFrame[moshFrameIndex].getTexture(), 1);
            shaderMoshMotion.setUniform2f("texelStep", 1.0f / MOSH_FRAME_WIDTH, 1.0f / MOSH_FRAME_HEIGHT);
            clipSpaceQuad.draw();
            shaderMoshMotion.end();
            moshMotion.end();
            moshFrame[nextFrame].getTexture().unbind(0);
            moshFrame[moshFrameIndex].getTexture().unbind(1);

            if(optimizationMode & 32) {
                GLboolean paramsBlend=glIsEnabled(GL_BLEND);
                glDisable(GL_BLEND); // Alpha stores a blend coefficient, not opacity.
                moshParams.begin();
                shaderMoshParams.begin();
                shaderMoshParams.setUniformTexture("texMotion",moshMotion.getTexture(),2);
                shaderMoshParams.setUniform2f("motionFrameSize",MOSH_FRAME_WIDTH,MOSH_FRAME_HEIGHT);
                shaderMoshParams.setUniform2f("motionFieldSize",MOSH_FIELD_WIDTH,MOSH_FIELD_HEIGHT);
                shaderMoshParams.setUniform1f("residual",expf(logf(0.02f)*moshAmount));
                shaderMoshParams.setUniform1f("vectorScale",1.0f+moshAmount);
                shaderMoshParams.setUniform1f("refreshChance",0.05f*moshAmount);
                shaderMoshParams.setUniform1f("frameIndex",static_cast<float>(moshFrameCount));
                clipSpaceQuad.draw();
                shaderMoshParams.end();
                moshParams.end();
                if(paramsBlend) glEnable(GL_BLEND);
                moshMotion.getTexture().unbind(2);
            }
            ofShader& activeMosh=(optimizationMode & 32) ? shaderMoshPrepared : shaderMosh;
            int nextAccum = 1 - moshAccumIndex;
            moshAccum[nextAccum].begin();
            activeMosh.begin();
            activeMosh.setUniformTexture("texCurrent", outputFbo.getTexture(), 0);
            activeMosh.setUniformTexture("texAccum", moshAccum[moshAccumIndex].getTexture(), 1);
            activeMosh.setUniformTexture("texMotion", (optimizationMode & 32) ? moshParams.getTexture() : moshMotion.getTexture(), 2);
            activeMosh.setUniform2f("motionFrameSize", MOSH_FRAME_WIDTH, MOSH_FRAME_HEIGHT);
            activeMosh.setUniform2f("motionFieldSize", MOSH_FIELD_WIDTH, MOSH_FIELD_HEIGHT);
            activeMosh.setUniform1f("residual", expf(logf(0.02f) * moshAmount));
            activeMosh.setUniform1f("vectorScale", 1.0f + moshAmount);
            activeMosh.setUniform1f("refreshChance", 0.05f * moshAmount);
            activeMosh.setUniform1f("frameIndex", static_cast<float>(moshFrameCount++));
            clipSpaceQuad.draw();
            activeMosh.end();
            moshAccum[nextAccum].end();
            outputFbo.getTexture().unbind(0);
            moshAccum[moshAccumIndex].getTexture().unbind(1);
            ((optimizationMode & 32) ? moshParams : moshMotion).getTexture().unbind(2);
            moshAccumIndex = nextAccum;
        }
        moshFrameIndex = nextFrame;
        postSource = &moshAccum[moshAccumIndex];
    } else {
        moshPrimed = false;
    }

    passAudit.mark("mosh");
    // Each active post-FX pass reads one FBO and writes to the other.
    bool screenRendered=false;
    bool directAllowed=(optimizationMode & 16) && ofGetWidth()==width && ofGetHeight()==height;
    bool doPixel=pixelAmount>0.0001f;
    bool doCrush=crushAmount>0.0001f && !((optimizationMode & 4) && chromaAmount>0.0001f);
    auto verifyDirect=[&](ofShader& shader,ofFbo& target){
        std::ifstream request("/dev/shm/waaave_verify_direct");
        if(!request.good())return;
        request.close();std::remove("/dev/shm/waaave_verify_direct");
        ofPixels actual;actual.allocate(width,height,OF_PIXELS_RGBA);
        glReadPixels(0,0,width,height,GL_RGBA,GL_UNSIGNED_BYTE,actual.getData());
        target.begin();shader.begin();shader.setUniform1i("windowFlip",0);
        shader.setUniformTexture("tex0",postSource->getTexture(),0);
        clipSpaceQuad.draw();shader.end();target.end();
        target.draw(0,0,width,height);
        ofPixels expected;expected.allocate(width,height,OF_PIXELS_RGBA);
        glReadPixels(0,0,width,height,GL_RGBA,GL_UNSIGNED_BYTE,expected.getData());
        unsigned different=0;int maxError=0;
        for(size_t i=0;i<actual.size();++i){int e=std::abs(int(actual[i])-int(expected[i]));
            maxError=std::max(maxError,e);different+=e!=0;}
        ofLogNotice()<<"DIRECT_VERIFY max_error="<<maxError<<" different_channels="<<different;
        ofSaveImage(actual,"/tmp/waaave_direct.png");
    };
    int postTarget = 0;
    if(holdAmount > 0.0001f && framecount >= framebufferLength && shaderFrameHold.isLoaded()) {
        // Latch three prior ring slots at staggered times. A slot remains intact
        // for 29 further frames; our longest hold is shorter than that.
        const int writeSlot = abs(framebufferLength - framedelayoffset) - 1;
        const int recentSlot = (writeSlot + 1) % framebufferLength;
        const unsigned int period = 4 + static_cast<unsigned int>(holdAmount * 19.0f);
        for(int group=0; group<3; ++group) {
            if(holdFrameSlots[group] < 0 || framecount >= holdNextRefresh[group]) {
                const bool firstLatch = holdFrameSlots[group] < 0;
                holdFrameSlots[group] = firstLatch
                    ? (recentSlot + group * 5) % framebufferLength : recentSlot;
                const unsigned int duration = period + group * 2;
                holdNextRefresh[group] = framecount + (firstLatch
                    ? std::min(duration, 28u - static_cast<unsigned int>(group * 5)) : duration);
            }
        }
        postFxFramebuffer[postTarget].begin();
        shaderFrameHold.begin();
        shaderFrameHold.setUniformTexture("tex0", postSource->getTexture(), 0);
        shaderFrameHold.setUniformTexture("old0", pastFrames[holdFrameSlots[0]].getTexture(), 1);
        shaderFrameHold.setUniformTexture("old1", pastFrames[holdFrameSlots[1]].getTexture(), 2);
        shaderFrameHold.setUniformTexture("old2", pastFrames[holdFrameSlots[2]].getTexture(), 3);
        shaderFrameHold.setUniform2f("resolution", width, height);
        shaderFrameHold.setUniform1f("amount", holdAmount);
        shaderFrameHold.setUniform1f("elapsedSeconds", ofGetElapsedTimef());
        clipSpaceQuad.draw();
        shaderFrameHold.end();
        postFxFramebuffer[postTarget].end();
        postSource->getTexture().unbind(0);
        for(int group=0;group<3;++group)pastFrames[holdFrameSlots[group]].getTexture().unbind(group+1);
        postSource = &postFxFramebuffer[postTarget];
        postTarget = 1 - postTarget;
    } else if(holdAmount <= 0.0001f) {
        for(int group=0;group<3;++group)holdFrameSlots[group]=-1;
    }
    if(chromaAmount > 0.0001f) {
        bool direct=directAllowed && (!doCrush && !doPixel);
        if(!direct) postFxFramebuffer[postTarget].begin();
        ofShader& activeChroma=(optimizationMode & 4) ? shaderChromaCrush : shaderChroma;
        activeChroma.begin();
        activeChroma.setUniform1i("windowFlip",direct ? 1 : 0);
        activeChroma.setUniformTexture("tex0", postSource->getTexture(), 0);
        activeChroma.setUniform1f("amount", chromaAmount);
        if(optimizationMode & 4) activeChroma.setUniform1f("crushAmount",crushAmount);
        clipSpaceQuad.draw();
        activeChroma.end();
        if(!direct) postFxFramebuffer[postTarget].end();
        else {screenRendered=true;verifyDirect(activeChroma,postFxFramebuffer[postTarget]);}
        postSource->getTexture().unbind(0);
        if(!direct) postSource = &postFxFramebuffer[postTarget];
        postTarget = 1 - postTarget;
    }
    if(doCrush) {
        bool direct=directAllowed && (!doPixel);
        if(!direct) postFxFramebuffer[postTarget].begin();
        shaderCrush.begin();
        shaderCrush.setUniform1i("windowFlip",direct ? 1 : 0);
        shaderCrush.setUniformTexture("tex0", postSource->getTexture(), 0);
        shaderCrush.setUniform1f("amount", crushAmount);
        clipSpaceQuad.draw();
        shaderCrush.end();
        if(!direct) postFxFramebuffer[postTarget].end();
        else {screenRendered=true;verifyDirect(shaderCrush,postFxFramebuffer[postTarget]);}
        postSource->getTexture().unbind(0);
        if(!direct) postSource = &postFxFramebuffer[postTarget];
        postTarget = 1 - postTarget;
    }
    if(doPixel) {
        bool direct=directAllowed && (true);
        if(!direct) postFxFramebuffer[postTarget].begin();
        shaderPixel.begin();
        shaderPixel.setUniform1i("windowFlip",direct ? 1 : 0);
        shaderPixel.setUniformTexture("tex0", postSource->getTexture(), 0);
        shaderPixel.setUniform1f("amount", pixelAmount);
        shaderPixel.setUniform2f("resolution", width, height);
        clipSpaceQuad.draw();
        shaderPixel.end();
        if(!direct) postFxFramebuffer[postTarget].end();
        else {screenRendered=true;verifyDirect(shaderPixel,postFxFramebuffer[postTarget]);}
        postSource->getTexture().unbind(0);
        if(!direct) postSource = &postFxFramebuffer[postTarget];
    }
    passAudit.mark("postfx");
    if(!screenRendered) postSource->draw(0,0,width,height);
    {
        std::ifstream capture("/dev/shm/waaave_capture_once");
        if(capture.good()) {
            capture.close(); std::remove("/dev/shm/waaave_capture_once");
            ofPixels captured;
            if(screenRendered){captured.allocate(width,height,OF_PIXELS_RGBA);
                glReadPixels(0,0,width,height,GL_RGBA,GL_UNSIGNED_BYTE,captured.getData());}
            else postSource->readToPixels(captured);
            ofSaveImage(captured,"/tmp/waaave_output.png");
            ofLogNotice()<<"OUTPUT_CAPTURE /tmp/waaave_output.png";
        }
    }

    const unsigned long overlayNow = ofGetElapsedTimeMillis();
    if(showOverlayFadeMs == 0) showOverlayFadeMs = overlayNow;
    const float overlayStep = 0.65f * ofClamp(
        (overlayNow - showOverlayFadeMs) / 1000.0f, 0.0f, 0.1f);
    showOverlayFadeMs = overlayNow;
    if(showOverlayRequestedMode != showOverlayMode) {
        showOverlayAlpha = std::max(0.0f, showOverlayAlpha - overlayStep);
        if(showOverlayAlpha <= 0.001f) {
            showOverlayMode = showOverlayRequestedMode;
            showOverlayAlpha = 0.0f;
        }
    } else if(showOverlayMode != 0) {
        showOverlayAlpha = std::min(1.0f, showOverlayAlpha + overlayStep);
    } else {
        showOverlayAlpha = 0.0f;
    }

    ofImage* showOverlay = nullptr;
    if(showOverlayMode == 1) showOverlay = &houseCardImage;
    else if(showOverlayMode == 2) showOverlay = &houseLogoImage;
    else if(showOverlayMode == 3) showOverlay = &mainLogoImage;
    if(showOverlay && showOverlay->isAllocated() && showOverlayAlpha > 0.0f) {
        ofEnableAlphaBlending();
        ofSetColor(255, 255, 255, static_cast<int>(showOverlayAlpha * 255.0f));
        if(showOverlayMode <= 2) {
            const float overlayWidth = width * 0.6f;
            const float overlayHeight = ofGetHeight() * 0.6f;
            showOverlay->draw((width - overlayWidth) * 0.5f,
                              (ofGetHeight() - overlayHeight) * 0.5f,
                              overlayWidth, overlayHeight);
        } else {
            showOverlay->draw(0, 0, width, height);
        }
        ofDisableAlphaBlending();
        ofSetColor(255, 255, 255, 255);
    }

    blackoutSmoothed += (ofClamp(blackoutAmount + forceCcOffset[41], 0.0f, 1.0f)
                         - blackoutSmoothed) * 0.08f;
    if(blackoutSmoothed > 0.004f){
        ofEnableAlphaBlending();
        ofSetColor(0, 0, 0, (int)(blackoutSmoothed * 255));
        ofDrawRectangle(0, 0, width, height);
        ofSetColor(255, 255, 255, 255);
    }
    // Exchange storage instead of copying an opaque image. Preserve the draw
    // path for blend modes where the copy changes the destination.
    passAudit.mark("display");
    ofFbo& historyTarget=pastFrames[abs(framebufferLength-framedelayoffset)-1];
    GLint srcRgb=0,dstRgb=0,eqRgb=0,srcAlpha=0,dstAlpha=0,eqAlpha=0;
    glGetIntegerv(GL_BLEND_SRC_RGB,&srcRgb); glGetIntegerv(GL_BLEND_DST_RGB,&dstRgb);
    glGetIntegerv(GL_BLEND_EQUATION_RGB,&eqRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA,&srcAlpha); glGetIntegerv(GL_BLEND_DST_ALPHA,&dstAlpha);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA,&eqAlpha);
    // Mixer/sharpen alpha is one for the RGB input and opaque history used here.
    // Under the standard alpha blend, copying it overwrites the target exactly.
    bool opaqueCopy=!glIsEnabled(GL_BLEND) ||
        (srcRgb==GL_SRC_ALPHA && dstRgb==GL_ONE_MINUS_SRC_ALPHA && eqRgb==GL_FUNC_ADD &&
         (srcAlpha==GL_SRC_ALPHA || srcAlpha==GL_ONE) &&
         dstAlpha==GL_ONE_MINUS_SRC_ALPHA && eqAlpha==GL_FUNC_ADD);
    bool verifyHistory=false;
    ofPixels expectedHistory;
    {std::ifstream verify("/dev/shm/waaave_verify_history");verifyHistory=verify.good();}
    if(verifyHistory && wet_dry_switch==1) outputFbo.readToPixels(expectedHistory);
    if((optimizationMode & 1) && wet_dry_switch==1 && opaqueCopy) {
        std::swap(historyTarget,outputFbo);
    } else {
        historyTarget.begin();
        if(wet_dry_switch==0) {
            if(inputswitch==1) {
                if(hdmi_aspect_ratio_switch==0) cam1.draw(0,0,width,height);
                else aspect_fix_fbo.draw(0,0,width,height);
            }
            dry_framebuffer.begin();
            outputFbo.draw(0,0);
            dry_framebuffer.end();
        } else outputFbo.draw(0,0);
        historyTarget.end();
    }

    if(verifyHistory) {
        std::remove("/dev/shm/waaave_verify_history");
        if(wet_dry_switch==1) {
            ofPixels actualHistory;historyTarget.readToPixels(actualHistory);
            bool identical=actualHistory.size()==expectedHistory.size() &&
                std::equal(actualHistory.begin(),actualHistory.end(),expectedHistory.begin());
            ofSaveImage(expectedHistory,"/tmp/history_expected.png");
            ofSaveImage(actualHistory,"/tmp/history_actual.png");
            ofLogNotice()<<"HISTORY_VERIFY width="<<actualHistory.getWidth()<<" channels="<<actualHistory.getNumChannels()<<" expected_channels="<<expectedHistory.getNumChannels()<<" exact="<<identical<<" exchange="<<((optimizationMode & 1) && opaqueCopy);
        }
    }
    passAudit.mark("history");
    previewSource = &historyTarget;
    incIndex();
    //ofDrawBitmapString("fps =" + ofToString(ofGetFrameRate())+"fft low =" + ofToString(fftLow)+"fft low_smoothed =" + ofToString(low_value_smoothed)+"fft mid =" + ofToString(fftLow)+"fft mid_smoothed =" + ofToString(mid_value_smoothed), 10, ofGetHeight() - 5 );
    /*ofDrawBitmapString("fft high =" + ofToString(fftHigh,2)+
     "fft high_smoothed =" + ofToString(high_value_smoothed,2)+
     "fft mid =" + ofToString(fftMid,2)+
     "fft mid_smoothed =" + ofToString(mid_value_smoothed,2)+
     "fft low =" + ofToString(fftLow,2)+
     "fft low_smoothed =" + ofToString(low_value_smoothed,2)
     , 10, ofGetHeight() - 5 );
     */
    
    /*
     ofDrawBitmapString(
     "fft high =" + ofToString(high_value_smoothed,2)+
     "fft mid =" + ofToString(mid_value_smoothed,2)+
     "fft low =" + ofToString(low_value_smoothed,2)
     , 10, ofGetHeight() - 5 );
     */
    
    
    //i use this block of code to print out like useful information for debugging various things and/or just to keep the
    //framerate displayed to make sure i'm not losing any frames while testing out new features.  uncomment the ofDrawBitmap etc etc
    //to print the stuff out on screen
    // Lightweight measured-FPS heartbeat for field diagnostics. It does not
    // draw into the video output and emits only once per second.
    static unsigned int perfFrameCount = 0;
    static float perfLastTime = 0.0f;
    ++perfFrameCount;
    const float perfNow = ofGetElapsedTimef();
    if(perfLastTime == 0.0f) {
        perfLastTime = perfNow;
    } else if(perfNow - perfLastTime >= 1.0f) {
        const float measuredFps = perfFrameCount / (perfNow - perfLastTime);
        internalPreviewFps = measuredFps;
        ofLogNotice() << "PERF fps=" << measuredFps
                      << " reported=" << ofGetFrameRate();
        static int fpsSocket = -1;
        if(fpsSocket < 0) fpsSocket = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
        if(fpsSocket >= 0) {
            sockaddr_in target{};
            target.sin_family = AF_INET;
            target.sin_port = htons(5005);
            inet_pton(AF_INET, "127.0.0.1", &target.sin_addr);
            char packet[12];
            const int length = snprintf(packet, sizeof(packet), "FPS:%d",
                                        std::max(0, std::min(999, int(measuredFps + 0.5f))));
            sendto(fpsSocket, packet, length, MSG_DONTWAIT,
                   reinterpret_cast<const sockaddr*>(&target), sizeof(target));
        }
        perfFrameCount = 0;
        perfLastTime = perfNow;
    }
    ofSetColor(255);
    string msg="fps="+ofToString(ofGetFrameRate(),2)+" clear switch"+ofToString(clear_switch,5);//+" z="+ofToString(az,5);
    //ofDrawBitmapString(msg,10,10);
}

//--------------------------------------------------------------
void ofApp::exit() {
    if(forceCcSocket >= 0) close(forceCcSocket);
    // clean up
    midiIn.closePort();
    midiIn.removeListener(this);
}
//--------------------------------------------------------------
/*
 
 void ofApp::omx_settings(){
 
 settings.sensorWidth = 640;
 settings.sensorHeight = 480;
 settings.framerate = 30;
 settings.enableTexture = true;
 settings.sensorMode=7;
 
 settings.whiteBalance ="Off";
 settings.exposurePreset ="Off";
 settings.whiteBalanceGainR = 1.0;
 settings.whiteBalanceGainB = 1.0;
 
 }
 //------------------------------------------------------------
 
 void ofApp::omx_updates(){
 
 videoGrabber.setSharpness(50);
 videoGrabber.setBrightness(40);
 videoGrabber.setContrast(100);
 videoGrabber.setSaturation(0);
 
 }
 */
//--------------------------------------------------------------


void ofApp::saveAudioPatch(int slot) {
    if (!autoeyez::validStoredAudioSlot(slot)) {
        ofLogWarning() << "AUDIO_PATCH rejected saveAudioPatch slot=" << slot;
        return;
    }
    try{audioPatchText(slot);}catch(const std::exception& e){ofLogError()<<"AUDIO_PATCH invalid save: "<<e.what();return;}
    autoeyez::FftBands selectedBands=autoeyez::FftBands::Contiguous;
    if(!autoeyez::selectedBandProfile(selectedBands)) {
        ofLogWarning() << "AUDIO_PATCH rejected invalid Teensy band selection";
        return;
    }
    fftEnvelopeRuntime.poll(ofGetElapsedTimeMillis(), true);
    string path = "/home/pi/audiopatches/AUDIO" + ofToString(slot, 0, 2, '0') + ".TXT" ;
    const string temporary = path + "." + std::to_string(getpid()) + ".tmp";
    ofFile file(temporary, ofFile::WriteOnly) ;
    if (!file.is_open()) {
        ofLogError() << "AUDIO_PATCH save failed: " << path;
        return;
    }
    file << "audio_version=2\nslot=" << slot << "\n";
    for (int i = 1 ; i <= 26 ; i++) {
        float lv, mv, hv ;
        switch(i) {
            case 1:  lv=lowC1;  mv=midC1;  hv=highC1;  break ;
            case 2:  lv=lowC2;  mv=midC2;  hv=highC2;  break ;
            case 3:  lv=lowC3;  mv=midC3;  hv=highC3;  break ;
            case 4:  lv=lowC4;  mv=midC4;  hv=highC4;  break ;
            case 5:  lv=lowC5;  mv=midC5;  hv=highC5;  break ;
            case 6:  lv=lowC6;  mv=midC6;  hv=highC6;  break ;
            case 7:  lv=lowC7;  mv=midC7;  hv=highC7;  break ;
            case 8:  lv=lowC8;  mv=midC8;  hv=highC8;  break ;
            case 9:  lv=lowC9;  mv=midC9;  hv=highC9;  break ;
            case 10: lv=lowC10; mv=midC10; hv=highC10; break ;
            case 11: lv=lowC11; mv=midC11; hv=highC11; break ;
            case 12: lv=lowC12; mv=midC12; hv=highC12; break ;
            case 13: lv=lowC13; mv=midC13; hv=highC13; break ;
            case 14: lv=lowC14; mv=midC14; hv=highC14; break ;
            case 15: lv=lowC15; mv=midC15; hv=highC15; break ;
            case 16: lv=lowC16; mv=midC16; hv=highC16; break ;
            case 17: lv=lowC17; mv=midC17; hv=highC17; break ;
            case 18: lv=lowC18; mv=midC18; hv=highC18; break ;
            case 19: lv=lowC19; mv=midC19; hv=highC19; break ;
            case 20: lv=lowC20; mv=midC20; hv=highC20; break ;
            case 21: case 22: case 23: case 24: case 25:
                lv=inputProcessingDepth[0][i-21];
                mv=inputProcessingDepth[1][i-21];
                hv=inputProcessingDepth[2][i-21]; break ;
            case 26: lv=holdDepth[0]; mv=holdDepth[1]; hv=holdDepth[2]; break ;
            default: lv=mv=hv=0; break ;
        }
        file << "low" << i << "=" << lv << "\n" ;
        file << "mid" << i << "=" << mv << "\n" ;
        file << "high" << i << "=" << hv << "\n" ;
    }
    file << "react=" << audioReactiveControlSwitch << "\n" ;
    autoeyez::writeEnvelopePatch(file, fftEnvelopeRuntime.envelope.config);
    autoeyez::writeBandPatch(file, selectedBands);
    file.close() ;
    if(file.fail() || std::rename(temporary.c_str(), path.c_str()) != 0) {
        std::remove(temporary.c_str());
        ofLogError() << "AUDIO_PATCH save failed: " << path;
        return;
    }
    ofLogNotice() << "AUDIO_PATCH saved slot=" << slot << " path=" << path;
}

void ofApp::applyAudioPatchText(const std::string& text,int slot){
    autoeyez::AudioPatch patch;
    if(!autoeyez::readAudioPatch(text,slot,patch))throw std::runtime_error("invalid audio patch schema");
    lowC1=patch.coefficients[0][0];
    lowC2=patch.coefficients[0][1];
    lowC3=patch.coefficients[0][2];
    lowC4=patch.coefficients[0][3];
    lowC5=patch.coefficients[0][4];
    lowC6=patch.coefficients[0][5];
    lowC7=patch.coefficients[0][6];
    lowC8=patch.coefficients[0][7];
    lowC9=patch.coefficients[0][8];
    lowC10=patch.coefficients[0][9];
    lowC11=patch.coefficients[0][10];
    lowC12=patch.coefficients[0][11];
    lowC13=patch.coefficients[0][12];
    lowC14=patch.coefficients[0][13];
    lowC15=patch.coefficients[0][14];
    lowC16=patch.coefficients[0][15];
    lowC17=patch.coefficients[0][16];
    lowC18=patch.coefficients[0][17];
    lowC19=patch.coefficients[0][18];
    lowC20=patch.coefficients[0][19];
    midC1=patch.coefficients[1][0];
    midC2=patch.coefficients[1][1];
    midC3=patch.coefficients[1][2];
    midC4=patch.coefficients[1][3];
    midC5=patch.coefficients[1][4];
    midC6=patch.coefficients[1][5];
    midC7=patch.coefficients[1][6];
    midC8=patch.coefficients[1][7];
    midC9=patch.coefficients[1][8];
    midC10=patch.coefficients[1][9];
    midC11=patch.coefficients[1][10];
    midC12=patch.coefficients[1][11];
    midC13=patch.coefficients[1][12];
    midC14=patch.coefficients[1][13];
    midC15=patch.coefficients[1][14];
    midC16=patch.coefficients[1][15];
    midC17=patch.coefficients[1][16];
    midC18=patch.coefficients[1][17];
    midC19=patch.coefficients[1][18];
    midC20=patch.coefficients[1][19];
    highC1=patch.coefficients[2][0];
    highC2=patch.coefficients[2][1];
    highC3=patch.coefficients[2][2];
    highC4=patch.coefficients[2][3];
    highC5=patch.coefficients[2][4];
    highC6=patch.coefficients[2][5];
    highC7=patch.coefficients[2][6];
    highC8=patch.coefficients[2][7];
    highC9=patch.coefficients[2][8];
    highC10=patch.coefficients[2][9];
    highC11=patch.coefficients[2][10];
    highC12=patch.coefficients[2][11];
    highC13=patch.coefficients[2][12];
    highC14=patch.coefficients[2][13];
    highC15=patch.coefficients[2][14];
    highC16=patch.coefficients[2][15];
    highC17=patch.coefficients[2][16];
    highC18=patch.coefficients[2][17];
    highC19=patch.coefficients[2][18];
    highC20=patch.coefficients[2][19];
    for(int band=0;band<3;++band)
        for(int i=0;i<5;++i)
            inputProcessingDepth[band][i]=patch.coefficients[band][20+i];
    for(int band=0;band<3;++band)holdDepth[band]=patch.coefficients[band][25];
    audioReactiveControlSwitch=patch.reactive;
    if(!fftEnvelopeRuntime.load(patch.envelope,ofGetElapsedTimeMillis())||!autoeyez::requestBandProfile(patch.bands))throw std::runtime_error("controller profile request failed");
}
void ofApp::loadAudioPatch(int slot){
    if(!autoeyez::validStoredAudioSlot(slot))return;
    std::ifstream file("/home/pi/audiopatches/AUDIO"+ofToString(slot,0,2,'0')+".TXT");
    file.seekg(0,std::ios::end);const auto length=file.tellg();if(length<0||length>16384){ofLogError()<<"AUDIO_PATCH file size rejected";return;}file.seekg(0);
    std::ostringstream text;text<<file.rdbuf();
    try{if(!file)throw std::runtime_error("audio patch file unavailable");applyAudioPatchText(text.str(),slot);ofLogNotice()<<"AUDIO_PATCH loaded slot="<<slot;}
    catch(const std::exception& e){ofLogError()<<"AUDIO_PATCH rejected slot="<<slot<<" reason="<<e.what();}
}

void ofApp::fftAssignValues(){
    enum class FftSource { Serial, Pi, Midi };
    static FftSource selected = FftSource::Serial;
    static FftSource lastSource = FftSource::Serial;
    static unsigned long lastSourceCheck = 0;
    const unsigned long nowMs = ofGetElapsedTimeMillis();
    if(nowMs - lastSourceCheck >= 250) {
        std::ifstream sourceFile("/dev/shm/fft_source");
        std::string source;
        if(sourceFile >> source) {
            if(source == "MIDI" || source == "midi") selected = FftSource::Midi;
            else if(source == "PI" || source == "pi") selected = FftSource::Pi;
            else selected = FftSource::Serial;
        }
        lastSourceCheck = nowMs;
        if(selected != lastSource) {
            ofLogNotice() << "FFT_SOURCE " <<
                (selected == FftSource::Midi ? "MIDI" :
                 selected == FftSource::Pi ? "PI" : "TEENSY");
            lastSource = selected;
        }
    }

    std::array<std::uint8_t, 3> midiValues{{0, 0, 0}};
    std::array<std::uint16_t, 3> midiExtended{{0, 0, 0}};
    bool midiHasExtended = false;
    std::uint64_t midiFrames = 0;
    bool midiValid = false;
    std::chrono::steady_clock::time_point midiAt{};
    {
        std::lock_guard<std::mutex> lock(midiFftMutex);
        midiValues = midiFftFrame.values;
        midiExtended = midiFftFrame.extendedValues;
        midiHasExtended = midiFftFrame.hasExtended;
        midiFrames = midiFftFrame.frames;
        midiValid = midiFftFrame.valid;
        midiAt = midiFftAt;
    }
    const auto midiAge = midiValid ? std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - midiAt).count() : -1;
    const bool midiFresh = midiValid && midiAge >= 0 && midiAge <= 200;
    static bool wasMidiFallback = false;
    const bool midiFallback = selected == FftSource::Midi && !midiFresh;
    if(midiFallback != wasMidiFallback) {
        ofLogWarning() << "FFT_MIDI fallback=" << (midiFallback ? "PI" : "OFF")
                       << " age_ms=" << midiAge;
        wasMidiFallback = midiFallback;
    }

    float rawLow, rawMid, rawHigh;
    if(selected == FftSource::Midi && midiFresh) {
        // The extended CCs retain FFT peaks above the legacy 7-bit ceiling.
        // Old firmware continues to use the original CC frame unchanged.
        rawLow = midiHasExtended ? midiExtended[0] * (32.0f / 16383.0f)
                                 : midiValues[0] / 127.0f;
        rawMid = midiHasExtended ? midiExtended[1] * (32.0f / 16383.0f)
                                 : midiValues[1] / 127.0f;
        rawHigh = midiHasExtended ? midiExtended[2] * (32.0f / 16383.0f)
                                  : midiValues[2] / 127.0f;
    } else if(selected == FftSource::Serial) {
        int low = 0, mid = 0, high = 0;
        char comma1 = 0, comma2 = 0;
        std::ifstream values("/dev/shm/fft_values");
        if(values >> low >> comma1 >> mid >> comma2 >> high && comma1 == ',' && comma2 == ',') {
            rawLow = ofClamp(low / 127.0f, 0.0f, 1.0f);
            rawMid = ofClamp(mid / 127.0f, 0.0f, 1.0f);
            rawHigh = ofClamp(high / 127.0f, 0.0f, 1.0f);
        } else {
            // The bridge writes space-separated values after parsing serial.
            values.clear(); values.seekg(0);
            if(values >> low >> mid >> high) {
                rawLow = ofClamp(low / 127.0f, 0.0f, 1.0f);
                rawMid = ofClamp(mid / 127.0f, 0.0f, 1.0f);
                rawHigh = ofClamp(high / 127.0f, 0.0f, 1.0f);
            } else rawLow = rawMid = rawHigh = 0.0f;
        }
    } else {
        rawLow = fft.getLowVal(); rawMid = fft.getMidVal(); rawHigh = fft.getHighVal();
    }
    const float inputTrimGain = std::pow(
        10.0f, (12.0f * forceCcOffset[98]) / 20.0f);
    if(selected == FftSource::Pi || midiFallback) {
        fftLow = ofClamp(rawLow * inputGain * inputTrimGain, 0.0f, range) / range;
        fftMid = ofClamp(rawMid * inputGain * inputTrimGain, 0.0f, range) / range;
        fftHigh = ofClamp(rawHigh * inputGain * inputTrimGain, 0.0f, range) / range;
    } else {
        fftLow = ofClamp(rawLow * inputTrimGain, 0.0f, 1.0f);
        fftMid = ofClamp(rawMid * inputTrimGain, 0.0f, 1.0f);
        fftHigh = ofClamp(rawHigh * inputTrimGain, 0.0f, 1.0f);
    }

    static unsigned long lastMidiReport = 0;
    static std::uint64_t previousMidiFrames = 0;
    if(nowMs - lastMidiReport >= 5000) {
        ofLogNotice() << "FFT_MIDI frames_5s=" << midiFrames - previousMidiFrames
                      << " age_ms=" << midiAge
                      << " active=" << (selected == FftSource::Midi && midiFresh);
        previousMidiFrames = midiFrames;
        lastMidiReport = nowMs;
    }

    fftEnvelopeRuntime.poll(nowMs);
    static double lastEnvelopeSeconds = -1;
    const double envelopeSeconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const double dt = lastEnvelopeSeconds < 0 ? 1.0/30.0 : envelopeSeconds-lastEnvelopeSeconds;
    lastEnvelopeSeconds = envelopeSeconds;
    const auto smoothed = fftEnvelopeRuntime.envelope.update({{fftLow,fftMid,fftHigh}},dt);
    low_value_smoothed=smoothed[0];mid_value_smoothed=smoothed[1];high_value_smoothed=smoothed[2];

    // Shader reactivity has its own envelope and headroom. The legacy
    // renderer parameters retain their 0..1 behavior and saved settings.
    static autoeyez::FftEnvelope shaderFftEnvelope;
    shaderFftEnvelope.config=fftEnvelopeRuntime.envelope.config;
    const float shaderInputScale=(selected==FftSource::Pi || midiFallback)
        ? inputGain*inputTrimGain/range : inputTrimGain;
    // Gentle compression preserves movement above unity without pushing
    // geometry or animation rate to an unusable extreme at high Force gain.
    const float shaderScale=1.0f/std::asinh(1.0f);
    const auto shaderSmoothed=shaderFftEnvelope.update(
        {{std::asinh(std::max(0.0f,rawLow*shaderInputScale))*shaderScale,
          std::asinh(std::max(0.0f,rawMid*shaderInputScale))*shaderScale,
          std::asinh(std::max(0.0f,rawHigh*shaderInputScale))*shaderScale}},
        dt,8.0f);

    // Autoclip receives the same shaped audio bands used by this renderer.
    // UDP rides the existing wired link; no FFT traffic is added to serial.
    static int shaderFftSocket = -1;
    static sockaddr_in shaderFftTarget{};
    static unsigned long lastShaderFftSend = 0;
    if(shaderFftSocket < 0) {
        shaderFftSocket = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
        shaderFftTarget.sin_family = AF_INET;
        shaderFftTarget.sin_port = htons(5006);
        inet_pton(AF_INET, "10.0.0.2", &shaderFftTarget.sin_addr);
    }
    if(shaderFftSocket >= 0 && nowMs - lastShaderFftSend >= 20) {
        char packet[48];
        const int length = snprintf(packet, sizeof(packet), "SHADER_FFT:%d,%d,%d",
            int(shaderSmoothed[0] * 127.0f + 0.5f),
            int(shaderSmoothed[1] * 127.0f + 0.5f),
            int(shaderSmoothed[2] * 127.0f + 0.5f));
        sendto(shaderFftSocket, packet, length, MSG_DONTWAIT,
               reinterpret_cast<const sockaddr*>(&shaderFftTarget),
               sizeof(shaderFftTarget));
        lastShaderFftSend = nowMs;
    }

    // Peak and mean readings distinguish a live signal from capture that is
    // merely open. This also shows whether the legacy range=200 is suitable.
    static float peakLow = 0.0f, peakMid = 0.0f, peakHigh = 0.0f;
    static float sumLow = 0.0f, sumMid = 0.0f, sumHigh = 0.0f;
    static int sampleCount = 0;
    static unsigned long lastReport = 0;
    peakLow = std::max(peakLow, rawLow);
    peakMid = std::max(peakMid, rawMid);
    peakHigh = std::max(peakHigh, rawHigh);
    sumLow += rawLow;
    sumMid += rawMid;
    sumHigh += rawHigh;
    ++sampleCount;
    unsigned long now = ofGetElapsedTimeMillis();
    if(now - lastReport >= 5000 && sampleCount > 0) {
        ofLogNotice() << "FFT_LEVEL mean=" << sumLow/sampleCount << ","
                      << sumMid/sampleCount << "," << sumHigh/sampleCount
                      << " peak=" << peakLow << "," << peakMid << "," << peakHigh
                      << " scaled=" << low_value_smoothed << ","
                      << mid_value_smoothed << "," << high_value_smoothed;
        peakLow = peakMid = peakHigh = 0.0f;
        sumLow = sumMid = sumHigh = 0.0f;
        sampleCount = 0;
        lastReport = now;
    }
}
//-------------------------------------------------------------
void ofApp::parametersAssign(){
    // Derive both profiles each frame: dry mode must not alter later wet frames.
    c_hue = wet_dry_switch ? .25f : 1.0f;
    c_sat = wet_dry_switch ? .2f : 1.0f;
    c_bright = wet_dry_switch ? .2f : 1.0f;
    c_x = wet_dry_switch ? .01f : .1f;
    c_y = wet_dry_switch ? .01f : .1f;
    c_z = wet_dry_switch ? .05f : .5f;
    
    d_delay=(abs(framedelayoffset-framebufferLength-fb0_delayamount-
                 int((manualControlSmoothed[15]+lowC16*low_value_smoothed+midC16*mid_value_smoothed+highC16*high_value_smoothed)*(framebufferLength-1.0))
                 )-1)%framebufferLength;
    // One Force sweep covers the same tap span as the physical delay knob.
    const int forceDelayFrames = static_cast<int>(std::round(
        forceCcOffset[40] * (framebufferLength - 1.0f)));
    d_delay = (d_delay + forceDelayFrames % framebufferLength +
               framebufferLength) % framebufferLength;
    d_lumakey_value=kk+c_lumakey_value*manualControlSmoothed[0]+4.0f*(
                                                                lowC1*low_value_smoothed+
                                                                midC1*mid_value_smoothed+
                                                                highC1*high_value_smoothed);
    d_mix=jm+c_mix*manualControlSmoothed[1]+4.0f*(
                                            lowC2*low_value_smoothed+
                                            midC2*mid_value_smoothed+
                                            highC2*high_value_smoothed);
    d_hue=fv*(1.0f+c_hue*manualControlSmoothed[2])+4.0f*(
                                                   lowC3*low_value_smoothed+
                                                   midC3*mid_value_smoothed+
                                                   highC3*high_value_smoothed);
    d_sat=gb*(1.0f+c_sat*manualControlSmoothed[3])+4.0f*(
                                                   lowC4*low_value_smoothed+
                                                   midC4*mid_value_smoothed+
                                                   highC4*high_value_smoothed);
    d_bright=hn*(1.0f+c_bright*manualControlSmoothed[4])+4.0f*(
                                                         lowC5*low_value_smoothed+
                                                         midC5*mid_value_smoothed+
                                                         highC5*high_value_smoothed);
    d_temporalFilterMix=op+c_temporalFilterMix*manualControlSmoothed[5]+4.0f*(
                                                                        lowC6*low_value_smoothed+
                                                                        midC6*mid_value_smoothed+
                                                                        highC6*high_value_smoothed);
    d_temporalFilterResonance=fb1_brightx+manualControlSmoothed[6]+4.0*(
                                                                  lowC7*low_value_smoothed+
                                                                  midC7*mid_value_smoothed+
                                                                  highC7*high_value_smoothed);
    d_sharpenAmount=ll+c_sharpenAmount*manualControlSmoothed[7]+8.0f*(
                                                                lowC8*low_value_smoothed+
                                                                midC8*mid_value_smoothed+
                                                                highC8*high_value_smoothed);
    d_x=sx+c_x*manualControlSmoothed[8]+4.0f*(
                                        c_x*(lowC9*low_value_smoothed+
                                             midC9*mid_value_smoothed+
                                             highC9*high_value_smoothed));
    d_y=dc+c_y*manualControlSmoothed[9]+4.0f*(
                                        c_y*(lowC10*low_value_smoothed+
                                             midC10*mid_value_smoothed+
                                             highC10*high_value_smoothed));
    d_z=az*(1.0f+c_z*manualControlSmoothed[10])+4.0f*(
                                                c_z*(lowC11*low_value_smoothed+
                                                     midC11*mid_value_smoothed+
                                                     highC11*high_value_smoothed));
    d_rotate=qw+c_rotate*manualControlSmoothed[11]+4.0*(
                                                  c_rotate*(lowC12*low_value_smoothed+
                                                            midC12*mid_value_smoothed+
                                                            highC12*high_value_smoothed));
    d_huex_mod=er*(1.0f-manualControlSmoothed[12])+4.0f*(
                                                   lowC13*low_value_smoothed+
                                                   midC13*mid_value_smoothed+
                                                   highC13*high_value_smoothed);
    d_huex_off=ty+c_huex_off*manualControlSmoothed[13]+4.0f*(
                                                       c_huex_off*(lowC14*low_value_smoothed+
                                                                   midC14*mid_value_smoothed+
                                                                   highC14*high_value_smoothed));
    d_huex_lfo=ui+c_huex_lfo*manualControlSmoothed[14]+4.0f*(
                                                       c_huex_lfo*(lowC15*low_value_smoothed+
                                                                   midC15*mid_value_smoothed+
                                                                   highC15*high_value_smoothed));

    // Force Q-Link layer: live offsets added after the saved patch and audio
    // values are resolved. CC value 63 returns each parameter to its patch value.
    d_lumakey_value += forceCcOffset[16];
    if(lumaTrackEnabled && lumaTrackMeasurementReady && !lumaTrackReferencePending) {
        // Follow scene exposure while preserving the manually chosen luma
        // threshold as the artistic baseline captured when tracking is enabled.
        d_lumakey_value += ofClamp(lumaTrackSmoothed - lumaTrackReference,
                                   -0.75f, 0.75f);
    }
    d_mix += 2.0f * forceCcOffset[17];
    d_hue += forceCcOffset[18];
    d_sat += forceCcOffset[19];
    d_bright += forceCcOffset[20];
    d_temporalFilterMix += forceCcOffset[21];
    d_temporalFilterResonance += forceCcOffset[25];
    d_sharpenAmount += forceCcOffset[23];
    const float xRange = x_2 ? 2.0f : x_5 ? 5.0f : x_10 ? 10.0f : 1.0f;
    const float yRange = y_2 ? 2.0f : y_5 ? 5.0f : y_10 ? 10.0f : 1.0f;
    const float rotateRange = r_2 ? 2.0f : r_5 ? 5.0f : r_10 ? 10.0f : 1.0f;
    const float hueModDivisor = huexx_0 ? 64.0f : huexx_1 ? 96.0f :
                                 huexx_2 ? 127.0f : 32.0f;
    const float hueOffsetRange = huexy_0 ? 2.0f : huexy_1 ? 4.0f :
                                  huexy_2 ? 8.0f : 1.0f;
    const float hueLfoRange = huexz_0 ? 2.0f : huexz_1 ? 4.0f :
                               huexz_2 ? 8.0f : 1.0f;
    d_x += c_x * xRange * forceCcOffset[26];
    d_y += c_y * yRange * forceCcOffset[27];
    d_rotate += c_rotate * rotateRange * forceCcOffset[28];
    d_huex_mod -= er * (127.0f / hueModDivisor) * forceCcOffset[29];
    d_huex_off += c_huex_off * hueOffsetRange * forceCcOffset[30];
    d_huex_lfo += c_huex_lfo * hueLfoRange * forceCcOffset[31];
    // Match the Teensy's selected 1x/2x/5x/10x zoom range.
    float zoomRange = z_2 ? 2.0f : z_5 ? 5.0f : z_10 ? 10.0f : 1.0f;
    d_z = std::max(0.1f, d_z + az * c_z * zoomRange * forceCcOffset[122]);
}
//--------------------------------------------------------------
void ofApp::newMidiMessage(ofxMidiMessage& msg) {
    midiLastMessageMs.store(ofGetElapsedTimeMillis());
    if(msg.status == MIDI_CONTROL_CHANGE && msg.channel == 1 &&
       msg.control >= 110 && msg.control <= 119) {
        std::lock_guard<std::mutex> lock(midiFftMutex);
        if(midiFftFrame.accept(msg.control, msg.value))
            midiFftAt = std::chrono::steady_clock::now();
        return;
    }
    std::lock_guard<std::mutex> lock(midiMessagesMutex);
    midiBurstQueue.push(midiMessages, msg, ofGetElapsedTimeMillis(),
                        !midiPreserveRepeatedControls.load());
}
void ofApp::expirePatchWriteLatch(unsigned long now) {
    if(autoeyez::patchLatchExpired(forceMidiWrite, now, forceMidiWriteTimer)) {
        forceMidiWrite=false;
        forceMidiWriteTimer=0;
        midiLatchClear();
        ofLogWarning()<<"MIDI_LATCH expired after missing release";
    }
}
//------------------------------------------------------------
void ofApp::updateManualControls(){
    for(int i=0;i<manualControlCount;i++){
        const bool spatialControl = (i >= 8 && i <= 11);
        const float smooth = spatialControl ? transform_control_smooth : manualControlSmoothing;
        manualControlSmoothed[i]=manualControlTargets[i]*(1.0f-smooth)+manualControlSmoothed[i]*smooth;
        if(!spatialControl && abs(manualControlSmoothed[i])<.05){manualControlSmoothed[i]=0;}
    }
}
//-------------------------------------------------------------
void ofApp::inputUpdate(){
    if(inputswitch==1){
        cam1.update();
        //corner crop and stretch to preserve hd aspect ratio
        if(hdmi_aspect_ratio_switch==1){
            aspect_fix_fbo.begin();
            cam1.draw(0,0,width,height);
            aspect_fix_fbo.end();
        }
    }
    if(inputswitch==0){
        //omx_updates();
    }
}
//------------------------------------------------------------
void ofApp::pollLumaTrackState(){
    const unsigned long now = ofGetElapsedTimeMillis();
    if(now - lumaTrackStatePollMs < 100) return;
    lumaTrackStatePollMs = now;

    std::ifstream stateFile("/dev/shm/luma_track");
    int requested = 1;
    if(!(stateFile >> requested)) requested = 1; // Default ON after boot.
    const bool nextEnabled = requested != 0;
    if(nextEnabled == lumaTrackEnabled) return;

    lumaTrackEnabled = nextEnabled;
    lumaTrackReferencePending = nextEnabled;
    ofLogNotice() << "LUMA_TRACK " << (nextEnabled ? "ON" : "OFF");
}
//------------------------------------------------------------
void ofApp::updateLumaTrackMeasurement(){
    const ofPixels& pixels = cam1.getPixels();
    if(!pixels.isAllocated() || pixels.getWidth() < 1 || pixels.getHeight() < 1) return;

    const int channels = pixels.getNumChannels();
    if(channels < 3) return;

    // The grabber can report initial black buffers while its stream starts.
    // Wait half a second of input before capturing the default-ON baseline.
    if(lumaTrackInputFrames < 15) {
        ++lumaTrackInputFrames;
        return;
    }

    constexpr int gridWidth = 32;
    constexpr int gridHeight = 18;
    std::vector<float> luminance;
    luminance.reserve(gridWidth * gridHeight);
    const unsigned char* data = pixels.getData();
    const int sourceWidth = pixels.getWidth();
    const int sourceHeight = pixels.getHeight();

    for(int gy = 0; gy < gridHeight; ++gy) {
        const int y = std::min(sourceHeight - 1,
            ((2 * gy + 1) * sourceHeight) / (2 * gridHeight));
        for(int gx = 0; gx < gridWidth; ++gx) {
            const int x = std::min(sourceWidth - 1,
                ((2 * gx + 1) * sourceWidth) / (2 * gridWidth));
            const std::size_t offset =
                (static_cast<std::size_t>(y) * sourceWidth + x) * channels;
            const float r = data[offset] / 255.0f;
            const float g = data[offset + 1] / 255.0f;
            const float b = data[offset + 2] / 255.0f;
            // Match shader_mixer's rgb2hsb(...).z (HSV value). A weighted
            // luma estimate under-reacts to saturated red and blue scenes.
            luminance.push_back(std::max(r, std::max(g, b)));
        }
    }

    std::sort(luminance.begin(), luminance.end());
    const std::size_t trim = luminance.size() / 10;
    float sum = 0.0f;
    for(std::size_t i = trim; i < luminance.size() - trim; ++i) sum += luminance[i];
    const float measured = sum / static_cast<float>(luminance.size() - 2 * trim);

    if(!lumaTrackMeasurementReady) {
        lumaTrackSmoothed = measured;
        lumaTrackMeasurementReady = true;
    } else {
        // Roughly 100-150 ms response at a 30 fps source: quick scene
        // adaptation without making cuts or flashes chatter the threshold.
        lumaTrackSmoothed += 0.24f * (measured - lumaTrackSmoothed);
    }

    if(lumaTrackEnabled && lumaTrackReferencePending) {
        lumaTrackSmoothed = measured;
        lumaTrackReference = measured;
        lumaTrackReferencePending = false;
        ofLogNotice() << "LUMA_TRACK reference=" << lumaTrackReference;
    }

    const unsigned long now = ofGetElapsedTimeMillis();
    if(lumaTrackEnabled && now - lumaTrackLogMs >= 5000) {
        lumaTrackLogMs = now;
        ofLogNotice() << "LUMA_TRACK level=" << lumaTrackSmoothed
                      << " reference=" << lumaTrackReference
                      << " offset=" << (lumaTrackSmoothed - lumaTrackReference);
    }
}
//------------------------------------------------------------
void ofApp::clearManualControls(){
    for(int i=0;i<manualControlCount;i++){
        manualControlTargets[i]=0;
    }
}
//------------------------------------------------------------
void ofApp::midiSetup(){
    // print input ports to console
    midiIn.listInPorts();

    if(!openPreferredMidiPort()) {
        ofLogWarning() << "No preferred MIDI controller found; will retry.";
        return;
    }

    // Snapshot the bridge identity at initial open. A later USB generation
    // forces re-subscription even when ALSA recycles the same address/name.
    std::ifstream generationFile("/dev/shm/autoeyez_controller_generation");
    generationFile >> midiSerialGeneration;

    // don't ignore sysex, timing, & active sense messages,
    // these are ignored by default
    midiIn.ignoreTypes(false, false, false);
    
    // add ofApp as a listener
    midiIn.addListener(this);
    
    // print received messages to the console
    midiIn.setVerbose(false);
}
//-------------------------------------------------------------
void ofApp::forceCcSetup() {
    forceCcSocket = socket(AF_INET, SOCK_DGRAM, 0);
    if(forceCcSocket < 0) {
        ofLogError() << "FORCE_CC socket creation failed";
        return;
    }
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(5010);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if(bind(forceCcSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        ofLogError() << "FORCE_CC UDP port 5010 bind failed";
        close(forceCcSocket);
        forceCcSocket = -1;
        return;
    }
    fcntl(forceCcSocket, F_SETFL, fcntl(forceCcSocket, F_GETFL, 0) | O_NONBLOCK);
    ofLogNotice() << "FORCE_CC listening on UDP 5010";
}
//-------------------------------------------------------------
void ofApp::forceCcPoll() {
    if(forceCcSocket < 0) return;
    unsigned char packet[4];
    sockaddr_in sender = {};
    socklen_t senderSize = sizeof(sender);
    for(int i = 0; i < 64; ++i) {
        ssize_t size = recvfrom(forceCcSocket, packet, sizeof(packet), 0,
                                reinterpret_cast<sockaddr*>(&sender), &senderSize);
        if(size < 0) break;
        if(size != 3 || ntohl(sender.sin_addr.s_addr) != 0x0a000002 ||
           (packet[0] & 0xf0) != 0xb0 || packet[1] > 127 || packet[2] > 127) continue;
        int cc = packet[1];
        if(cc == 93) {
            int slot = packet[2];
            if(slot >= 1 && slot <= 32) {
                loadAudioPatch(slot);
                ofLogNotice() << "FORCE_CC audio patch=" << slot;
            } else {
                ofLogWarning() << "FORCE_CC ignored audio patch=" << slot;
            }
            continue;
        }
        switch(cc) {
            case 16: case 17: case 18: case 19: case 20: case 21: case 23:
            case 25: case 26: case 27: case 28: case 29: case 30: case 31:
            case 40: case 41:
            case 94: case 95: case 96: case 97: case 98: case 122:
                // 63 is exact center. Both endpoints reach full offset.
                forceCcOffset[cc] = packet[2] <= 63
                    ? (static_cast<float>(packet[2]) - 63.0f) / 63.0f
                    : (static_cast<float>(packet[2]) - 63.0f) / 64.0f;
                {
                    static unsigned long lastLog = 0;
                    unsigned long now = ofGetElapsedTimeMillis();
                    if(now - lastLog >= 1000) {
                        ofLogNotice() << "FORCE_CC cc=" << cc << " value="
                                      << static_cast<int>(packet[2]) << " offset="
                                      << forceCcOffset[cc];
                        lastLog = now;
                    }
                }
                break;
            default: break;
        }
    }
}
//-------------------------------------------------------------
void ofApp::pollShowOverlayState() {
    const unsigned long now = ofGetElapsedTimeMillis();
    if(now - showOverlayPollMs < 100) return;
    showOverlayPollMs = now;
    std::ifstream state("/dev/shm/show_overlay");
    int requested = 0;
    if(!(state >> requested)) requested = 0;
    requested = ofClamp(requested, 0, 3);
    if(requested != showOverlayRequestedMode) {
        showOverlayRequestedMode = requested;
        ofLogNotice() << "SHOW_OVERLAY requested=" << showOverlayRequestedMode;
    }
}
//-------------------------------------------------------------
bool ofApp::openPreferredMidiPort(){
    const auto ports=midiIn.getInPortList();
    for(size_t i=0;i<ports.size();++i){
        // Support the current Nano controller and the named Teensy/automidi
        // devices used in the installed system. Do not assume a port index.
        if(ports[i].find("nanoKONTROL2") != string::npos ||
           ports[i].find("Teensy") != string::npos ||
           ports[i].find("automidi") != string::npos){
            ofLogNotice() << "Opening MIDI input: " << ports[i];
            return midiIn.openPort(i);
        }
    }
    return false;
}
//-------------------------------------------------------------
void ofApp::fbDeclareAndAllocate(){
    allocateColorFbo(framebuffer0, width,height);
    framebuffer0.begin();
    // DEBUG: print texture dimensions once
    static bool printed = false;
    if(!printed && cam1.isInitialized()) {
        ofLogNotice() << "TEX_DEBUG cam1=" << cam1.getTexture().getWidth() << "x" << cam1.getTexture().getHeight()
                      << " fb0=" << framebuffer0.getWidth() << "x" << framebuffer0.getHeight();
        printed = true;
    }
    ofClear(0,0,0,255);
    framebuffer0.end();
    
    allocateColorFbo(aspect_fix_fbo, width,height);
    aspect_fix_fbo.begin();
    ofClear(0,0,0,255);
    aspect_fix_fbo.end();
    
    allocateColorFbo(dry_framebuffer, width,height);
    dry_framebuffer.begin();
    ofClear(0,0,0,255);
    dry_framebuffer.end();
    
    allocateColorFbo(sharpenFramebuffer, width,height);
    sharpenFramebuffer.begin();
    ofClear(0,0,0,255);
    sharpenFramebuffer.end();
    for (ofFbo& postFx : postFxFramebuffer) {
        allocateColorFbo(postFx, width,height);
        postFx.begin();
        ofClear(0,0,0,255);
        postFx.end();
    }
    for (ofFbo& inputFx : inputProcessingFramebuffer) {
        allocateColorFbo(inputFx, inputWidth, inputHeight);
        inputFx.begin();
        ofClear(0,0,0,255);
        inputFx.end();
    }
    for (ofFbo& frame : moshFrame) {
        allocateColorFbo(frame, MOSH_FRAME_WIDTH, MOSH_FRAME_HEIGHT);
        frame.begin();
        ofClear(0,0,0,255);
        frame.end();
    }
    allocateColorFbo(moshParams, MOSH_FIELD_WIDTH,MOSH_FIELD_HEIGHT,GL_RGBA32F);
    moshParams.getTexture().setTextureMinMagFilter(GL_NEAREST,GL_NEAREST);
    allocateColorFbo(moshMotion, MOSH_FIELD_WIDTH, MOSH_FIELD_HEIGHT);
    moshMotion.begin();
    ofClear(128,128,0,255);
    moshMotion.end();
    for (ofFbo& accum : moshAccum) {
        allocateColorFbo(accum, width,height);
        accum.begin();
        ofClear(0,0,0,255);
        accum.end();
    }
    
    for(int i=0;i<framebufferLength;i++){
        allocateColorFbo(pastFrames[i], width, height);
        pastFrames[i].begin();
        ofClear(0,0,0,255);
        pastFrames[i].end();
    }
}
//-------------------------------------------------------------
void ofApp::publishAppStatus() {
    const auto elapsed = ofGetElapsedTimeMillis();
    if(elapsed - lastStatusPublishMs < 1000) return;
    lastStatusPublishMs = elapsed;
    const auto monoMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const std::string path = "/dev/shm/autoeyez_app_status.json";
    const std::string temporary = path + "." + std::to_string(getpid()) + ".tmp";
    std::ofstream status(temporary);
    if(!status) return;
    status << "{\"pid\":" << getpid() << ",\"mono_ms\":" << monoMs
           << ",\"video_ready\":" << (videoReady ? "true" : "false")
           << ",\"input_frames\":" << appInputFrames << "}\n";
    status.close();
    if(status) std::rename(temporary.c_str(), path.c_str());
}
//--------------------------------------------------------------
bool ofApp::loadVideoShaders() {
    struct ShaderAsset { ofShader* shader; const char* path; };
    const ShaderAsset assets[] = {
        {&shader_mixer, "shadersES2/shader_mixer"},
        {&shaderOptimizedMixer, "shadersES2/shader_mixer_optimized"},
        {&shaderChromaCrush, "shadersES2/shaderChromaCrush"},
        {&shaderMoshParams, "shadersES2/shaderMoshParams"},
        {&shaderMoshPrepared, "shadersES2/shaderMoshPrepared"},
        {&shaderPassthrough, "shadersES2/passthrough"},
        {&shaderSharpen, "shadersES2/shaderSharpen"},
        {&shaderPixel, "shadersES2/shaderPixel"},
        {&shaderChroma, "shadersES2/shaderChroma"},
        {&shaderInputProcessing, "shadersES2/shaderInputProcessing"},
        {&shaderFrameHold, "shadersES2/shaderFrameHold"},
        {&shaderCrush, "shadersES2/shaderCrush"},
        {&shaderMoshMotion, "shadersES2/shaderMoshMotion"},
        {&shaderMosh, "shadersES2/shaderMosh"},
    };
    bool loaded = true;
    for(const auto& asset : assets) {
        const std::string path = asset.path;
        bool success = ofFile::doesFileExist(path + ".vert", true)
                       && ofFile::doesFileExist(path + ".frag", true)
                       && asset.shader->load(path);
        // This SDK's load() ignores stage-compile return values. Require
        // both actual compiled stages, as well as a successfully linked program.
        if(success) {
            for(GLenum stage : {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER}) {
                const GLuint id = asset.shader->getShader(stage);
                GLint compiled = GL_FALSE;
                if(id) glGetShaderiv(id, GL_COMPILE_STATUS, &compiled);
                success = success && compiled == GL_TRUE;
            }
        }
        if(!success) {
            loaded = false;
            ofLogError() << "VIDEO_SHADER failed " << path;
        }
    }
    if(!loaded) ofLogError() << "VIDEO_SHADERS failed; retrying in 4 seconds";
    return loaded;
}
//--------------------------------------------------------------
void ofApp::updateVideoInput() {
    auto now = ofGetElapsedTimeMillis();
    if(!videoShadersReady && now >= nextShaderRetryMs) {
        videoShadersReady = loadVideoShaders();
        nextShaderRetryMs = ofGetElapsedTimeMillis() + 4000;
    }
    if(!inputRecovery.initialized() && inputRecovery.canAttempt(now)
       && checkSocketExists()) {
        const bool success = inputSetup();
        // Setup can take time; start the first-frame timer when it finishes.
        now = ofGetElapsedTimeMillis();
        inputRecovery.setupCompleted(success, now);
        if(!success) {
            cam1.close();
            ofLogNotice() << "INPUT_RECOVER setup-failed retry_ms="
                          << inputRecovery.nextAttempt() - now;
        } else {
            ofLogNotice() << "INPUT_RECOVER waiting-for-first-frame";
        }
    }
    if(inputRecovery.initialized()) {
        inputUpdate();
        now = ofGetElapsedTimeMillis();
        if(cam1.isFrameNew()) {
            const bool firstFrame = !inputRecovery.hasFrame();
            ++auditInputs;
            ++appInputFrames;
            updateLumaTrackMeasurement();
            inputRecovery.frameArrived(now);
            if(firstFrame) ofLogNotice() << "INPUT_RECOVER first-frame";
        } else if(const char* reason = inputRecovery.timeout(now)) {
            cam1.close();
            now = ofGetElapsedTimeMillis();
            inputRecovery.failed(now);
            ofLogNotice() << "INPUT_RECOVER " << reason << " retry_ms="
                          << inputRecovery.nextAttempt() - now;
        }
    }
    videoReady = inputRecovery.initialized() && inputRecovery.hasFrame()
                 && videoShadersReady;
}
//--------------------------------------------------------------
bool ofApp::checkSocketExists() {
#ifdef AUTOEYEZ_D3_TRACE_CANDIDATE
    const char* traceFlag=getenv("AUTOEYEZ_TRACE_INPUT");
    if(traceFlag && std::string(traceFlag)=="1") return true;
#endif
    const char* socketPath = getenv("SHMSRC_PATH");
    if(socketPath == NULL) socketPath = "/tmp/video_shm";
    struct stat sb;
    return (stat(socketPath, &sb) == 0 && S_ISSOCK(sb.st_mode));
}
//-------------------------------------------------------------
void ofApp::drawLoading() {
    ofBackground(0);

    // Build loading text with animated dots
    string dots = "";
    for(int i = 0; i < loadingDotState; i++) dots += ".";
    string text = "LOADING" + dots;

    // Scale up for visibility (bitmap font is 8x14 pixels per char)
    float scale = 5.0;
    float charWidth = 8;
    float charHeight = 14;

    // Reserve space for max dots so LOADING doesn't shift
    float maxWidth = 10 * charWidth * scale;  // "LOADING..."
    float textHeight = charHeight * scale;

    ofPushMatrix();
    ofTranslate((width - maxWidth) / 2, (height - textHeight) / 2);
    ofScale(scale, scale);
    ofSetColor(255);
    ofDrawBitmapString(text, 0, charHeight);
    ofPopMatrix();
}
//-------------------------------------------------------------
bool ofApp::inputSetup(){
    if(!realFrameGrabberInstalled) {
#ifdef AUTOEYEZ_D3_TRACE_CANDIDATE
        const char* traceFlag=getenv("AUTOEYEZ_TRACE_INPUT");
        if(traceFlag && std::string(traceFlag)=="1") {
            traceInput=std::make_shared<TraceFrameGrabber>();cam1.setGrabber(traceInput);
        } else
#endif
        cam1.setGrabber(std::make_shared<RealFrameGrabber>());
        realFrameGrabberInstalled = true;
    }
    if(inputswitch != 1) return false;
    cam1.listDevices();
    cam1.setDeviceID(0);
    cam1.setDesiredFrameRate(30);
    return cam1.initGrabber(inputWidth, inputHeight);
}
//--------------------------------------------------------------
void ofApp::keyPressed(int key) {
    if (key == ')') {fft.setNormalize(true);}
    
    //error-if i decrement fb0_delayamount it always crashes...
    if (key == '[') {fb0_delayamount += 1;}
    if (key == ']') {
        fb0_delayamount = fb0_delayamount-1;
        if(fb0_delayamount<0){
            fb0_delayamount=framebufferLength-fb0_delayamount;
        }//endiffb0
    }//endifkey
    
    //fb1 mix
    if (key == 'o') {op += .01;}
    if (key == 'p') {op -= .01;}
    
    //fb0 z displace
    if (key == 'a') {az += .0001;}
    if (key == 'z') {az -= .0001;}
    
    //fb0 x displace
    if (key == 's') {sx += .0001;}
    if (key == 'x') {sx -= .0001;}
    
    //fb0 y displace
    if (key == 'd') {dc += .0001;}
    if (key == 'c') {dc -= .0001;}
    
    //fb0 hue attenuate
    if (key == 'f') {fv += .001;}
    if (key == 'v') {fv -= .001;}
    
    //fb0 saturation attenuate
    if (key == 'g') {gb += .001;}
    if (key == 'b') {gb -= .001;}
    
    //fb0 brightness attenuate
    if (key == 'h') {hn += .001;}
    if (key == 'n') {hn -= .001;}
    
    //fb0 mix
    if (key == 'j') {jm += .01;}
    if (key == 'm') {jm -= .01;}
    
    //fb0 lumakey value
    if (key == 'k') {kk = ofClamp(kk+.01,0.0,1.0);}
    if (key == ',') {kk = ofClamp(kk-.01,0.0,1.0);}
    
    
    if (key == 'l') {ll += .01;}
    if (key == '.') {ll -= .01;}
    
    if (key == ';') {fb1_brightx += .01;}
    if (key == '\'') {fb1_brightx -= .01;}
    
    //fb0 rotation
    if (key == 'q') {qw += .0001;}
    if (key == 'w') {qw -= .0001;}
    
    
    //hue chaos1
    if (key == 'e') {er += .001;}
    if (key == 'r') {er -= .001;}
    
    //hue chaos2
    if (key == 't') {ty += .01;}
    if (key == 'y') {ty -= .01;}
    
    //hue chaos3
    if (key == 'u') {ui += .01;}
    if (key == 'i') {ui -= .01;}
    
    if (key == '1') {
        //clear the framebuffer if thats whats up
        framebuffer0.begin();
    // DEBUG: print texture dimensions once
    static bool printed = false;
    if(!printed && cam1.isInitialized()) {
        ofLogNotice() << "TEX_DEBUG cam1=" << cam1.getTexture().getWidth() << "x" << cam1.getTexture().getHeight()
                      << " fb0=" << framebuffer0.getWidth() << "x" << framebuffer0.getHeight();
        printed = true;
    }
        ofClear(0, 0, 0, 255);
        framebuffer0.end();
        for(int i=0;i<framebufferLength;i++){
            pastFrames[i].begin();
            ofClear(0,0,0,255);
            pastFrames[i].end();
            
        }//endifor
    }
    
    if(key=='2'){brightInvert=!brightInvert;}
    if(key=='3'){hueInvert=!hueInvert;}
    if(key=='4'){saturationInvert=!saturationInvert;}
    
    if(key=='5'){verticalMirror=!verticalMirror;}
    if(key=='6'){horizontalMirror=!horizontalMirror;}
    
    if(key=='7'){toroidSwitch=!toroidSwitch;}
    
    if (key == '-') {y_skew += .01;}
    if (key == '=') {y_skew -= .01;}
    if (key == '9') {x_skew += .01;}
    if (key == '0') {x_skew -= .01;}
    
    //reset button
    if (key == '!') {
        az = 1.0;
        sx = 0;
        dc = 0;
        fv = 1;
        gb = 1;
        hn = 1;
        jm = 0.0;
        kk = 0.0;
        ll = 0.0;
        qw = 0.0;
        
        er = 1.0;
        ty = 0.0;
        ui = 0.0;
        
        op = 0.0;
        fb0_delayamount=1;
        
        brightInvert=0;
        hueInvert=0;
        saturationInvert=0;
        
        verticalMirror=0;
        horizontalMirror=0;
        
        x_skew=0;
        y_skew=0;
        
        framebuffer0.begin();
    // DEBUG: print texture dimensions once
    static bool printed = false;
    if(!printed && cam1.isInitialized()) {
        ofLogNotice() << "TEX_DEBUG cam1=" << cam1.getTexture().getWidth() << "x" << cam1.getTexture().getHeight()
                      << " fb0=" << framebuffer0.getWidth() << "x" << framebuffer0.getHeight();
        printed = true;
    }
        ofClear(0, 0, 0, 255);
        framebuffer0.end();
        
        for(int i=0;i<framebufferLength;i++){
            pastFrames[i].begin();
            ofClear(0,0,0,255);
            pastFrames[i].end();
        }//endifor
    }
}
// =============================================================================
// MIDI MESSAGE PROCESSING
// =============================================================================
// Processes incoming MIDI CC messages from automidi.
// Handles:
//   - Toggle switches (on/off states)
//   - Continuous controls with pick-up behavior
//   - Manual control pickup and smoothing
//   - Audio-reactive assignment
//   - Patch load/save commands
void ofApp::midibiz(const std::vector<ofxMidiMessage>* patchMessages){
    std::vector<ofxMidiMessage> pendingMessages;
    bool actionOverflow=false;
    const auto now=ofGetElapsedTimeMillis();
    if(patchMessages) pendingMessages=*patchMessages;
    else {
        std::lock_guard<std::mutex> lock(midiMessagesMutex);
        actionOverflow=midiBurstQueue.drain(midiMessages,pendingMessages,now);
        if(now-midiQueueReportMs>=5000 || actionOverflow || midiBurstQueue.stats.controlDiscarded) {
            const auto stats=midiBurstQueue.report();midiQueueReportMs=now;
            ofLogNotice()<<"MIDI_QUEUE received="<<stats.received<<" drained="<<stats.drained
                <<" filtered="<<stats.filtered<<" coalesced="<<stats.coalesced
                <<" control_discarded="<<stats.controlDiscarded<<" action_discarded="<<stats.actionDiscarded
                <<" high_water="<<stats.highWater<<" max_age_ms="<<stats.maxAgeMs;
        }
    }

    for(unsigned int i = 0; i < pendingMessages.size(); ++i) {

        ofxMidiMessage &message = pendingMessages[i];
        
        if(message.status < MIDI_SYSEX) {
            if(message.status == MIDI_CONTROL_CHANGE) {
                
                if(message.control==91){
                    if(message.value==127){
                        forceMidiWrite = true ;
                        forceMidiWriteTimer = ofGetElapsedTimeMillis() ;
                        midiLatchClear() ;
                        for(int i=0;i<manualControlCount;i++){
                            midiActiveFloat[i]=1 ;
                        }
                    }
                    if(message.value==0){
                        forceMidiWrite = false ;
                        forceMidiWriteTimer = 0 ;
                        
                        midiLatchClear() ;
                    }
                }
                if(message.control==92){
                    saveAudioPatch((int)message.value) ;
                }
                if(message.control==93){
                    loadAudioPatch((int)message.value) ;
                }
                if(message.control>=94 && message.control<=97) {
                    float* base = nullptr;
                    float* low = nullptr;
                    float* mid = nullptr;
                    float* high = nullptr;
                    if(message.control==94) {
                        base=&moshBase; low=&lowC17; mid=&midC17; high=&highC17;
                    } else if(message.control==95) {
                        base=&chromaBase; low=&lowC18; mid=&midC18; high=&highC18;
                    } else if(message.control==96) {
                        base=&crushBase; low=&lowC19; mid=&midC19; high=&highC19;
                    } else {
                        base=&pixelBase; low=&lowC20; mid=&midC20; high=&highC20;
                    }
                    float value = message.value / 127.0f;
                    if(audioReactiveControlSwitch==1) *low = value;
                    else if(audioReactiveControlSwitch==2) *mid = value;
                    else if(audioReactiveControlSwitch==3) *high = value;
                    else *base = value;
                }
                if(message.control>=102 && message.control<=106) {
                    const int effect=message.control-102;
                    const float value=message.value/127.0f;
                    if(audioReactiveControlSwitch>=1 && audioReactiveControlSwitch<=3)
                        inputProcessingDepth[audioReactiveControlSwitch-1][effect]=value;
                    else inputProcessingBase[effect]=value;
                }
                if(message.control==107) {
                    const float value=message.value/127.0f;
                    if(audioReactiveControlSwitch>=1 && audioReactiveControlSwitch<=3)
                        holdDepth[audioReactiveControlSwitch-1]=value;
                    else holdBase=value;
                }
                // Keep mid-batch expiry if an action takes time, plus the
                // unconditional frame check for silence/disconnection.
                expirePatchWriteLatch(ofGetElapsedTimeMillis());
                if(message.control==2){
                    blackoutAmount = message.value / 127.0f ;
                }
                if(message.control==7){
                    inputGain=ofMap(message.value,0,127,0.1f,4.0f);
                }
                if(message.control==39){
                    if(message.value==127){
                        videoReactiveSwitch=1;
                    }
                    if(message.value==0){
                        videoReactiveSwitch=0;
                    }
                }
                if(message.control==43){
                    if(message.value==127){
                        audioReactiveControlSwitch=1;
                    }
                    if(message.value==0){
                        audioReactiveControlSwitch=0;
                    }
                }
                if(message.control==44){
                    if(message.value==127){
                        audioReactiveControlSwitch=2;
                    }
                    if(message.value==0){
                        audioReactiveControlSwitch=0;
                    }
                }
                if(message.control==42){
                    if(message.value==127){
                        audioReactiveControlSwitch=3;
                    }
                    if(message.value==0){
                        audioReactiveControlSwitch=0;
                    }
                }
                
                if(message.control==32){
                    if(message.value==127){ x_2=true; }
                    if(message.value==0){ x_2=false; }
                }
                if(message.control==48){
                    if(message.value==127){ x_5=true; }
                    if(message.value==0){ x_5=false; }
                }
                if(message.control==64){
                    if(message.value==127){ x_10=true; }
                    if(message.value==0){ x_10=false; }
                }
                if(message.control==33){
                    if(message.value==127){ y_2=true; }
                    if(message.value==0){ y_2=false; }
                }
                if(message.control==49){
                    if(message.value==127){ y_5=true; }
                    if(message.value==0){ y_5=false; }
                }
                if(message.control==65){
                    if(message.value==127){ y_10=true; }
                    if(message.value==0){ y_10=false; }
                }
                if(message.control==34){
                    if(message.value==127){ z_2=true; }
                    if(message.value==0){ z_2=false; }
                }
                if(message.control==50){
                    if(message.value==127){ z_5=true; }
                    if(message.value==0){ z_5=false; }
                }
                if(message.control==66){
                    if(message.value==127){ z_10=true; }
                    if(message.value==0){ z_10=false; }
                }
                if(message.control==35){
                    if(message.value==127){ r_2=true; }
                    if(message.value==0){ r_2=false; }
                }
                if(message.control==51){
                    if(message.value==127){ r_5=true; }
                    if(message.value==0){ r_5=false; }
                }
                if(message.control==67){
                    if(message.value==127){ r_10=true; }
                    if(message.value==0){ r_10=false; }
                }
                if(message.control==36){
                    if(message.value==127){ huexx_0=true; }
                    if(message.value==0){ huexx_0=false; }
                }
                if(message.control==52){
                    if(message.value==127){ huexx_1=true; }
                    if(message.value==0){ huexx_1=false; }
                }
                if(message.control==68){
                    if(message.value==127){ huexx_2=true; }
                    if(message.value==0){ huexx_2=false; }
                }
                if(message.control==46){
                    if(message.value==127){ toroidSwitch=true; }
                    if(message.value==0){ toroidSwitch=false; }
                }
                if(message.control==101){
                    if(message.value==127){ mirrorSwitch=true; }
                    if(message.value==0){ mirrorSwitch=false; }
                }
                if(y_skew_switch==true){ y_skew+=.00001; }
                if(x_skew_switch==true){ x_skew+=.00001; }
                if(message.control==71){
                    if(message.value==127){ wet_dry_switch=false; }
                    if(message.value==0){ wet_dry_switch=true; }
                }
                if(message.control==37){
                    if(message.value==127){ huexy_0=true; }
                    if(message.value==0){ huexy_0=false; }
                }
                if(message.control==53){
                    if(message.value==127){ huexy_1=true; }
                    if(message.value==0){ huexy_1=false; }
                }
                if(message.control==69){
                    if(message.value==127){ huexy_2=true; }
                    if(message.value==0){ huexy_2=false; }
                }
                if(message.control==38){
                    if(message.value==127){ huexz_0=true; }
                    if(message.value==0){ huexz_0=false; }
                }
                if(message.control==54){
                    if(message.value==127){ huexz_1=true; }
                    if(message.value==0){ huexz_1=false; }
                }
                if(message.control==70){
                    if(message.value==127){ huexz_2=true; }
                    if(message.value==0){ huexz_2=false; }
                }
                if(message.control==100){
                    if(message.value==127){ lumakeyInvertSwitch=true; }
                    if(message.value==0){ lumakeyInvertSwitch=false; }
                }

                // ============================================================
                // CONTINUOUS CONTROLS
                // ============================================================

                // CC16 — lumakey (unipolar)
                if(message.control==16){
                    if(audioReactiveControlSwitch==0){
                        midiLowActiveFloat[0]=0;
                        midiMidActiveFloat[0]=0;
                        midiHighActiveFloat[0]=0;
                        {
                            vmidiActiveFloat[0]=0;
                            if(forceMidiWrite || abs(message.value/127.0f-manualControlTargets[0])<CONTROL_THRESHOLD){
                                midiActiveFloat[0]=1;
                            }
                            if(midiActiveFloat[0]==1){
                                manualControlTargets[0]=message.value/127.0f;
                            }
                        }
                        if(videoReactiveSwitch==1){
                            midiActiveFloat[0]=0;
                            if(forceMidiWrite || abs(message.value/127.0f-vLumakeyValue)<CONTROL_THRESHOLD){
                                vmidiActiveFloat[0]=1;
                            }
                            if(vmidiActiveFloat[0]==1){
                                vLumakeyValue=message.value/127.0f;
                            }
                        }
                    }
                    if(audioReactiveControlSwitch!=0){
                        midiActiveFloat[0]=0;
                        vmidiActiveFloat[0]=0;
                    }
                    if(audioReactiveControlSwitch==1){
                        midiMidActiveFloat[0]=0;
                        midiHighActiveFloat[0]=0;
                        if(forceMidiWrite || abs(message.value/127.0f-lowC1)<CONTROL_THRESHOLD){
                            midiLowActiveFloat[0]=1;
                        }
                        if(midiLowActiveFloat[0]==1){ lowC1=message.value/127.0f; }
                    }
                    if(audioReactiveControlSwitch==2){
                        midiLowActiveFloat[0]=0;
                        midiHighActiveFloat[0]=0;
                        if(forceMidiWrite || abs(message.value/127.0f-midC1)<CONTROL_THRESHOLD){
                            midiMidActiveFloat[0]=1;
                        }
                        if(midiMidActiveFloat[0]==1){ midC1=message.value/127.0f; }
                    }
                    if(audioReactiveControlSwitch==3){
                        midiLowActiveFloat[0]=0;
                        midiMidActiveFloat[0]=0;
                        if(forceMidiWrite || abs(message.value/127.0f-highC1)<CONTROL_THRESHOLD){
                            midiHighActiveFloat[0]=1;
                        }
                        if(midiHighActiveFloat[0]==1){ highC1=message.value/127.0f; }
                    }
                }

                // CC17 — mix (bipolar)
                if(message.control==17){
                    if(audioReactiveControlSwitch==0){
                        midiLowActiveFloat[1]=0;
                        midiMidActiveFloat[1]=0;
                        midiHighActiveFloat[1]=0;
                        {
                            vmidiActiveFloat[1]=0;
                            if(forceMidiWrite || abs((message.value-MIDI_MAGIC)/MIDI_MAGIC-manualControlTargets[1])<CONTROL_THRESHOLD){
                                midiActiveFloat[1]=1;
                            }
                            if(midiActiveFloat[1]==1){
                                manualControlTargets[1]=(message.value-MIDI_MAGIC)/MIDI_MAGIC;
                            }
                        }
                        if(videoReactiveSwitch==1){
                            midiActiveFloat[1]=0;
                            if(forceMidiWrite || abs((message.value-MIDI_MAGIC)/MIDI_MAGIC-vMix)<CONTROL_THRESHOLD){
                                vmidiActiveFloat[1]=1;
                            }
                            if(vmidiActiveFloat[1]==1){
                                vMix=(message.value-MIDI_MAGIC)/MIDI_MAGIC;
                            }
                        }
                    }
                    if(audioReactiveControlSwitch!=0){
                        midiActiveFloat[1]=0;
                        vmidiActiveFloat[1]=0;
                    }
                    if(audioReactiveControlSwitch==1){
                        midiMidActiveFloat[1]=0;
                        midiHighActiveFloat[1]=0;
                        if(forceMidiWrite || abs((message.value-MIDI_MAGIC)/MIDI_MAGIC-lowC2)<CONTROL_THRESHOLD){
                            midiLowActiveFloat[1]=1;
                        }
                        if(midiLowActiveFloat[1]==1){ lowC2=(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                    }
                    if(audioReactiveControlSwitch==2){
                        midiLowActiveFloat[1]=0;
                        midiHighActiveFloat[1]=0;
                        if(forceMidiWrite || abs((message.value-MIDI_MAGIC)/MIDI_MAGIC-midC2)<CONTROL_THRESHOLD){
                            midiMidActiveFloat[1]=1;
                        }
                        if(midiMidActiveFloat[1]==1){ midC2=(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                    }
                    if(audioReactiveControlSwitch==3){
                        midiLowActiveFloat[1]=0;
                        midiMidActiveFloat[1]=0;
                        if(forceMidiWrite || abs((message.value-MIDI_MAGIC)/MIDI_MAGIC-highC2)<CONTROL_THRESHOLD){
                            midiHighActiveFloat[1]=1;
                        }
                        if(midiHighActiveFloat[1]==1){ highC2=(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                    }
                }

                // CC18 — hue (bipolar)
                if(message.control==18){
                    if(audioReactiveControlSwitch==0){
                        midiLowActiveFloat[2]=0;
                        midiMidActiveFloat[2]=0;
                        midiHighActiveFloat[2]=0;
                        {
                            vmidiActiveFloat[2]=0;
                            if(forceMidiWrite || abs((message.value-MIDI_MAGIC)/MIDI_MAGIC-manualControlTargets[2])<CONTROL_THRESHOLD){
                                midiActiveFloat[2]=1;
                            }
                            if(midiActiveFloat[2]==1){
                                manualControlTargets[2]=(message.value-MIDI_MAGIC)/MIDI_MAGIC;
                            }
                        }
                        if(videoReactiveSwitch==1){
                            midiActiveFloat[2]=0;
                            if(forceMidiWrite || abs((message.value-MIDI_MAGIC)/MIDI_MAGIC-vHue)<CONTROL_THRESHOLD){
                                vmidiActiveFloat[2]=1;
                            }
                            if(vmidiActiveFloat[2]==1){
                                vHue=(message.value-MIDI_MAGIC)/MIDI_MAGIC;
                            }
                        }
                    }
                    if(audioReactiveControlSwitch!=0){
                        midiActiveFloat[2]=0;
                        vmidiActiveFloat[2]=0;
                    }
                    if(audioReactiveControlSwitch==1){
                        midiMidActiveFloat[2]=0;
                        midiHighActiveFloat[2]=0;
                        if(forceMidiWrite || abs((message.value-MIDI_MAGIC)/MIDI_MAGIC-lowC3)<CONTROL_THRESHOLD){
                            midiLowActiveFloat[2]=1;
                        }
                        if(midiLowActiveFloat[2]==1){ lowC3=(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                    }
                    if(audioReactiveControlSwitch==2){
                        midiLowActiveFloat[2]=0;
                        midiHighActiveFloat[2]=0;
                        if(forceMidiWrite || abs((message.value-MIDI_MAGIC)/MIDI_MAGIC-midC3)<CONTROL_THRESHOLD){
                            midiMidActiveFloat[2]=1;
                        }
                        if(midiMidActiveFloat[2]==1){ midC3=(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                    }
                    if(audioReactiveControlSwitch==3){
                        midiLowActiveFloat[2]=0;
                        midiMidActiveFloat[2]=0;
                        if(forceMidiWrite || abs((message.value-MIDI_MAGIC)/MIDI_MAGIC-highC3)<CONTROL_THRESHOLD){
                            midiHighActiveFloat[2]=1;
                        }
                        if(midiHighActiveFloat[2]==1){ highC3=(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                    }
                }

                // CC19 — saturation (bipolar)
                if(message.control==19){
                    if(audioReactiveControlSwitch==0){
                        midiLowActiveFloat[3]=0;
                        midiMidActiveFloat[3]=0;
                        midiHighActiveFloat[3]=0;
                        {
                            vmidiActiveFloat[3]=0;
                            if(forceMidiWrite || abs((message.value-MIDI_MAGIC)/MIDI_MAGIC-manualControlTargets[3])<CONTROL_THRESHOLD){
                                midiActiveFloat[3]=1;
                            }
                            if(midiActiveFloat[3]==1){
                                manualControlTargets[3]=(message.value-MIDI_MAGIC)/MIDI_MAGIC;
                            }
                        }
                        if(videoReactiveSwitch==1){
                            midiActiveFloat[3]=0;
                            if(forceMidiWrite || abs((message.value-MIDI_MAGIC)/MIDI_MAGIC-vSaturation)<CONTROL_THRESHOLD){
                                vmidiActiveFloat[3]=1;
                            }
                            if(vmidiActiveFloat[3]==1){
                                vSaturation=(message.value-MIDI_MAGIC)/MIDI_MAGIC;
                            }
                        }
                    }
                    if(audioReactiveControlSwitch!=0){
                        midiActiveFloat[3]=0;
                        vmidiActiveFloat[3]=0;
                    }
                    if(audioReactiveControlSwitch==1){
                        midiMidActiveFloat[3]=0;
                        midiHighActiveFloat[3]=0;
                        if(forceMidiWrite || abs((message.value-MIDI_MAGIC)/MIDI_MAGIC-lowC4)<CONTROL_THRESHOLD){
                            midiLowActiveFloat[3]=1;
                        }
                        if(midiLowActiveFloat[3]==1){ lowC4=(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                    }
                    if(audioReactiveControlSwitch==2){
                        midiLowActiveFloat[3]=0;
                        midiHighActiveFloat[3]=0;
                        if(forceMidiWrite || abs((message.value-MIDI_MAGIC)/MIDI_MAGIC-midC4)<CONTROL_THRESHOLD){
                            midiMidActiveFloat[3]=1;
                        }
                        if(midiMidActiveFloat[3]==1){ midC4=(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                    }
                    if(audioReactiveControlSwitch==3){
                        midiLowActiveFloat[3]=0;
                        midiMidActiveFloat[3]=0;
                        if(forceMidiWrite || abs((message.value-MIDI_MAGIC)/MIDI_MAGIC-highC4)<CONTROL_THRESHOLD){
                            midiHighActiveFloat[3]=1;
                        }
                        if(midiHighActiveFloat[3]==1){ highC4=(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                    }
                }

                // CC20 — brightness (bipolar)
                if(message.control==20){
                    if(audioReactiveControlSwitch==0){
                        midiLowActiveFloat[4]=0;
                        midiMidActiveFloat[4]=0;
                        midiHighActiveFloat[4]=0;
                        {
                            vmidiActiveFloat[4]=0;
                            if(forceMidiWrite || abs((message.value-MIDI_MAGIC)/MIDI_MAGIC-manualControlTargets[4])<CONTROL_THRESHOLD){
                                midiActiveFloat[4]=1;
                            }
                            if(midiActiveFloat[4]==1){
                                manualControlTargets[4]=(message.value-MIDI_MAGIC)/MIDI_MAGIC;
                            }
                        }
                        if(videoReactiveSwitch==1){
                            midiActiveFloat[4]=0;
                            if(forceMidiWrite || abs((message.value-MIDI_MAGIC)/MIDI_MAGIC-vBright)<CONTROL_THRESHOLD){
                                vmidiActiveFloat[4]=1;
                            }
                            if(vmidiActiveFloat[4]==1){
                                vBright=(message.value-MIDI_MAGIC)/MIDI_MAGIC;
                            }
                        }
                    }
                    if(audioReactiveControlSwitch!=0){
                        midiActiveFloat[4]=0;
                        vmidiActiveFloat[4]=0;
                    }
                    if(audioReactiveControlSwitch==1){
                        midiMidActiveFloat[4]=0;
                        midiHighActiveFloat[4]=0;
                        if(forceMidiWrite || abs((message.value-MIDI_MAGIC)/MIDI_MAGIC-lowC5)<CONTROL_THRESHOLD){
                            midiLowActiveFloat[4]=1;
                        }
                        if(midiLowActiveFloat[4]==1){ lowC5=(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                    }
                    if(audioReactiveControlSwitch==2){
                        midiLowActiveFloat[4]=0;
                        midiHighActiveFloat[4]=0;
                        if(forceMidiWrite || abs((message.value-MIDI_MAGIC)/MIDI_MAGIC-midC5)<CONTROL_THRESHOLD){
                            midiMidActiveFloat[4]=1;
                        }
                        if(midiMidActiveFloat[4]==1){ midC5=(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                    }
                    if(audioReactiveControlSwitch==3){
                        midiLowActiveFloat[4]=0;
                        midiMidActiveFloat[4]=0;
                        if(forceMidiWrite || abs((message.value-MIDI_MAGIC)/MIDI_MAGIC-highC5)<CONTROL_THRESHOLD){
                            midiHighActiveFloat[4]=1;
                        }
                        if(midiHighActiveFloat[4]==1){ highC5=(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                    }
                }

                // CC21 — temporal filter mix (bipolar)
                if(message.control==21){
                    if(audioReactiveControlSwitch==0){
                        midiLowActiveFloat[5]=0;
                        midiMidActiveFloat[5]=0;
                        midiHighActiveFloat[5]=0;
                        {
                            vmidiActiveFloat[5]=0;
                            if(forceMidiWrite || abs((message.value-MIDI_MAGIC)/MIDI_MAGIC-manualControlTargets[5])<CONTROL_THRESHOLD){
                                midiActiveFloat[5]=1;
                            }
                            if(midiActiveFloat[5]==1){
                                manualControlTargets[5]=(message.value-MIDI_MAGIC)/MIDI_MAGIC;
                            }
                        }
                        if(videoReactiveSwitch==1){
                            midiActiveFloat[5]=0;
                            if(forceMidiWrite || abs((message.value-MIDI_MAGIC)/MIDI_MAGIC-vTemporalFilterMix)<CONTROL_THRESHOLD){
                                vmidiActiveFloat[5]=1;
                            }
                            if(vmidiActiveFloat[5]==1){
                                vTemporalFilterMix=(message.value-MIDI_MAGIC)/MIDI_MAGIC;
                            }
                        }
                    }
                    if(audioReactiveControlSwitch!=0){
                        midiActiveFloat[5]=0;
                        vmidiActiveFloat[5]=0;
                    }
                    if(audioReactiveControlSwitch==1){
                        midiMidActiveFloat[5]=0;
                        midiHighActiveFloat[5]=0;
                        if(forceMidiWrite || abs((message.value-MIDI_MAGIC)/MIDI_MAGIC-lowC6)<CONTROL_THRESHOLD){
                            midiLowActiveFloat[5]=1;
                        }
                        if(midiLowActiveFloat[5]==1){ lowC6=(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                    }
                    if(audioReactiveControlSwitch==2){
                        midiLowActiveFloat[5]=0;
                        midiHighActiveFloat[5]=0;
                        if(forceMidiWrite || abs((message.value-MIDI_MAGIC)/MIDI_MAGIC-midC6)<CONTROL_THRESHOLD){
                            midiMidActiveFloat[5]=1;
                        }
                        if(midiMidActiveFloat[5]==1){ midC6=(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                    }
                    if(audioReactiveControlSwitch==3){
                        midiLowActiveFloat[5]=0;
                        midiMidActiveFloat[5]=0;
                        if(forceMidiWrite || abs((message.value-MIDI_MAGIC)/MIDI_MAGIC-highC6)<CONTROL_THRESHOLD){
                            midiHighActiveFloat[5]=1;
                        }
                        if(midiHighActiveFloat[5]==1){ highC6=(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                    }
                }

                // CC22 — temporal filter resonance (unipolar)
                if(message.control==22){
                    if(audioReactiveControlSwitch==0){
                        midiLowActiveFloat[6]=0;
                        midiMidActiveFloat[6]=0;
                        midiHighActiveFloat[6]=0;
                        {
                            vmidiActiveFloat[6]=0;
                            if(forceMidiWrite || abs(message.value/127.0f-manualControlTargets[6])<CONTROL_THRESHOLD){
                                midiActiveFloat[6]=1;
                            }
                            if(midiActiveFloat[6]==1){
                                manualControlTargets[6]=message.value/127.0f;
                            }
                        }
                        if(videoReactiveSwitch==1){
                            midiActiveFloat[6]=0;
                            if(forceMidiWrite || abs(message.value/127.0f-vTemporalFilterResonance)<CONTROL_THRESHOLD){
                                vmidiActiveFloat[6]=1;
                            }
                            if(vmidiActiveFloat[6]==1){
                                vTemporalFilterResonance=message.value/127.0f;
                            }
                        }
                    }
                    if(audioReactiveControlSwitch!=0){
                        midiActiveFloat[6]=0;
                        vmidiActiveFloat[6]=0;
                    }
                    if(audioReactiveControlSwitch==1){
                        midiMidActiveFloat[6]=0;
                        midiHighActiveFloat[6]=0;
                        if(forceMidiWrite || abs(message.value/127.0f-lowC7)<CONTROL_THRESHOLD){
                            midiLowActiveFloat[6]=1;
                        }
                        if(midiLowActiveFloat[6]==1){ lowC7=message.value/127.0f; }
                    }
                    if(audioReactiveControlSwitch==2){
                        midiLowActiveFloat[6]=0;
                        midiHighActiveFloat[6]=0;
                        if(forceMidiWrite || abs(message.value/127.0f-midC7)<CONTROL_THRESHOLD){
                            midiMidActiveFloat[6]=1;
                        }
                        if(midiMidActiveFloat[6]==1){ midC7=message.value/127.0f; }
                    }
                    if(audioReactiveControlSwitch==3){
                        midiLowActiveFloat[6]=0;
                        midiMidActiveFloat[6]=0;
                        if(forceMidiWrite || abs(message.value/127.0f-highC7)<CONTROL_THRESHOLD){
                            midiHighActiveFloat[6]=1;
                        }
                        if(midiHighActiveFloat[6]==1){ highC7=message.value/127.0f; }
                    }
                }

                // CC23 — sharpen (unipolar)
                if(message.control==23){
                    if(audioReactiveControlSwitch==0){
                        midiLowActiveFloat[7]=0;
                        midiMidActiveFloat[7]=0;
                        midiHighActiveFloat[7]=0;
                        {
                            vmidiActiveFloat[7]=0;
                            if(forceMidiWrite || abs(message.value/127.0f-manualControlTargets[7])<CONTROL_THRESHOLD){
                                midiActiveFloat[7]=1;
                            }
                            if(midiActiveFloat[7]==1){
                                manualControlTargets[7]=message.value/127.0f;
                            }
                        }
                        if(videoReactiveSwitch==1){
                            midiActiveFloat[7]=0;
                            if(forceMidiWrite || abs(message.value/127.0f-vSharpenAmount)<CONTROL_THRESHOLD){
                                vmidiActiveFloat[7]=1;
                            }
                            if(vmidiActiveFloat[7]==1){
                                vSharpenAmount=message.value/127.0f;
                            }
                        }
                    }
                    if(audioReactiveControlSwitch!=0){
                        midiActiveFloat[7]=0;
                        vmidiActiveFloat[7]=0;
                    }
                    if(audioReactiveControlSwitch==1){
                        midiMidActiveFloat[7]=0;
                        midiHighActiveFloat[7]=0;
                        if(forceMidiWrite || abs(message.value/127.0f-lowC8)<CONTROL_THRESHOLD){
                            midiLowActiveFloat[7]=1;
                        }
                        if(midiLowActiveFloat[7]==1){ lowC8=message.value/127.0f; }
                    }
                    if(audioReactiveControlSwitch==2){
                        midiLowActiveFloat[7]=0;
                        midiHighActiveFloat[7]=0;
                        if(forceMidiWrite || abs(message.value/127.0f-midC8)<CONTROL_THRESHOLD){
                            midiMidActiveFloat[7]=1;
                        }
                        if(midiMidActiveFloat[7]==1){ midC8=message.value/127.0f; }
                    }
                    if(audioReactiveControlSwitch==3){
                        midiLowActiveFloat[7]=0;
                        midiMidActiveFloat[7]=0;
                        if(forceMidiWrite || abs(message.value/127.0f-highC8)<CONTROL_THRESHOLD){
                            midiHighActiveFloat[7]=1;
                        }
                        if(midiHighActiveFloat[7]==1){ highC8=message.value/127.0f; }
                    }
                }

                // CC120 — x displace (bipolar)
                if(message.control==120){
                    if(audioReactiveControlSwitch==0){
                        midiLowActiveFloat[8]=0;
                        midiMidActiveFloat[8]=0;
                        midiHighActiveFloat[8]=0;
                        {
                            vmidiActiveFloat[8]=0;
                            { float xRaw=(message.value-MIDI_MAGIC)/MIDI_MAGIC; float xMul=(x_2?2.0f:x_5?5.0f:x_10?10.0f:1.0f);
                            if(forceMidiWrite || abs(xRaw*xMul-manualControlTargets[8])<CONTROL_THRESHOLD*xMul){ midiActiveFloat[8]=1; }
                            if(midiActiveFloat[8]==1){ manualControlTargets[8]=xRaw*xMul; } }
                        }
                        if(videoReactiveSwitch==1){
                            midiActiveFloat[8]=0;
                            { float xRaw=(message.value-MIDI_MAGIC)/MIDI_MAGIC; float xMul=(x_2?2.0f:x_5?5.0f:x_10?10.0f:1.0f);
                            if(forceMidiWrite || abs(xRaw*xMul-vX)<CONTROL_THRESHOLD*xMul){ vmidiActiveFloat[8]=1; }
                            if(vmidiActiveFloat[8]==1){ vX=xRaw*xMul; } }
                        }
                    }
                    if(audioReactiveControlSwitch!=0){
                        midiActiveFloat[8]=0;
                        vmidiActiveFloat[8]=0;
                    }
                    if(audioReactiveControlSwitch==1){
                        midiMidActiveFloat[8]=0;
                        midiHighActiveFloat[8]=0;
                        if(forceMidiWrite || abs((message.value-MIDI_MAGIC)/MIDI_MAGIC-lowC9)<CONTROL_THRESHOLD){
                            midiLowActiveFloat[8]=1;
                        }
                        if(midiLowActiveFloat[8]==1){
                            lowC9=(message.value-MIDI_MAGIC)/MIDI_MAGIC;
                            if(x_2==true){ lowC9=2.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(x_5==true){ lowC9=5.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(x_10==true){ lowC9=10.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                        }
                    }
                    if(audioReactiveControlSwitch==2){
                        midiLowActiveFloat[8]=0;
                        midiHighActiveFloat[8]=0;
                        if(forceMidiWrite || abs((message.value-MIDI_MAGIC)/MIDI_MAGIC-midC9)<CONTROL_THRESHOLD){
                            midiMidActiveFloat[8]=1;
                        }
                        if(midiMidActiveFloat[8]==1){
                            midC9=(message.value-MIDI_MAGIC)/MIDI_MAGIC;
                            if(x_2==true){ midC9=2.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(x_5==true){ midC9=5.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(x_10==true){ midC9=10.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                        }
                    }
                    if(audioReactiveControlSwitch==3){
                        midiLowActiveFloat[8]=0;
                        midiMidActiveFloat[8]=0;
                        if(forceMidiWrite || abs((message.value-MIDI_MAGIC)/MIDI_MAGIC-highC9)<CONTROL_THRESHOLD){
                            midiHighActiveFloat[8]=1;
                        }
                        if(midiHighActiveFloat[8]==1){
                            highC9=(message.value-MIDI_MAGIC)/MIDI_MAGIC;
                            if(x_2==true){ highC9=2.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(x_5==true){ highC9=5.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(x_10==true){ highC9=10.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                        }
                    }
                }

                // CC121 — y displace (bipolar)
                if(message.control==121){
                    if(audioReactiveControlSwitch==0){
                        midiLowActiveFloat[9]=0;
                        midiMidActiveFloat[9]=0;
                        midiHighActiveFloat[9]=0;
                        {
                            vmidiActiveFloat[9]=0;
                            { float yRaw=(message.value-MIDI_MAGIC)/MIDI_MAGIC; float yMul=(y_2?2.0f:y_5?5.0f:y_10?10.0f:1.0f);
                            if(forceMidiWrite || abs(yRaw*yMul-manualControlTargets[9])<CONTROL_THRESHOLD*yMul){ midiActiveFloat[9]=1; }
                            if(midiActiveFloat[9]==1){ manualControlTargets[9]=yRaw*yMul; } }
                        }
                        if(videoReactiveSwitch==1){
                            midiActiveFloat[9]=0;
                            { float yRaw=(message.value-MIDI_MAGIC)/MIDI_MAGIC; float yMul=(y_2?2.0f:y_5?5.0f:y_10?10.0f:1.0f);
                            if(forceMidiWrite || abs(yRaw*yMul-vY)<CONTROL_THRESHOLD*yMul){ vmidiActiveFloat[9]=1; }
                            if(vmidiActiveFloat[9]==1){ vY=yRaw*yMul; } }
                        }
                    }
                    if(audioReactiveControlSwitch!=0){
                        midiActiveFloat[9]=0;
                        vmidiActiveFloat[9]=0;
                    }
                    if(audioReactiveControlSwitch==1){
                        midiMidActiveFloat[9]=0;
                        midiHighActiveFloat[9]=0;
                        if(forceMidiWrite || abs((message.value-MIDI_MAGIC)/MIDI_MAGIC-lowC10)<CONTROL_THRESHOLD){
                            midiLowActiveFloat[9]=1;
                        }
                        if(midiLowActiveFloat[9]==1){
                            lowC10=(message.value-MIDI_MAGIC)/MIDI_MAGIC;
                            if(y_2==true){ lowC10=2.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(y_5==true){ lowC10=5.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(y_10==true){ lowC10=10.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                        }
                    }
                    if(audioReactiveControlSwitch==2){
                        midiLowActiveFloat[9]=0;
                        midiHighActiveFloat[9]=0;
                        if(forceMidiWrite || abs((message.value-MIDI_MAGIC)/MIDI_MAGIC-midC10)<CONTROL_THRESHOLD){
                            midiMidActiveFloat[9]=1;
                        }
                        if(midiMidActiveFloat[9]==1){
                            midC10=(message.value-MIDI_MAGIC)/MIDI_MAGIC;
                            if(y_2==true){ midC10=2.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(y_5==true){ midC10=5.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(y_10==true){ midC10=10.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                        }
                    }
                    if(audioReactiveControlSwitch==3){
                        midiLowActiveFloat[9]=0;
                        midiMidActiveFloat[9]=0;
                        if(forceMidiWrite || abs((message.value-MIDI_MAGIC)/MIDI_MAGIC-highC10)<CONTROL_THRESHOLD){
                            midiHighActiveFloat[9]=1;
                        }
                        if(midiHighActiveFloat[9]==1){
                            highC10=(message.value-MIDI_MAGIC)/MIDI_MAGIC;
                            if(y_2==true){ highC10=2.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(y_5==true){ highC10=5.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(y_10==true){ highC10=10.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                        }
                    }
                }

                // CC122 — z displace (bipolar)
                if(message.control==122){
                    if(audioReactiveControlSwitch==0){
                        midiLowActiveFloat[10]=0;
                        midiMidActiveFloat[10]=0;
                        midiHighActiveFloat[10]=0;
                        {
                            vmidiActiveFloat[10]=0;
                            { float zRaw=(message.value-MIDI_MAGIC)/MIDI_MAGIC; float zMul=(z_2?2.0f:z_5?5.0f:z_10?10.0f:1.0f);
                            if(forceMidiWrite || abs(zRaw*zMul-manualControlTargets[10])<CONTROL_THRESHOLD*zMul){ midiActiveFloat[10]=1; }
                            if(midiActiveFloat[10]==1){ manualControlTargets[10]=zRaw*zMul; } }
                        }
                        if(videoReactiveSwitch==1){
                            midiActiveFloat[10]=0;
                            { float zRaw=(message.value-MIDI_MAGIC)/MIDI_MAGIC; float zMul=(z_2?2.0f:z_5?5.0f:z_10?10.0f:1.0f);
                            if(forceMidiWrite || abs(zRaw*zMul-vZ)<CONTROL_THRESHOLD*zMul){ vmidiActiveFloat[10]=1; }
                            if(vmidiActiveFloat[10]==1){ vZ=zRaw*zMul; } }
                        }
                    }
                    if(audioReactiveControlSwitch!=0){
                        midiActiveFloat[10]=0;
                        vmidiActiveFloat[10]=0;
                    }
                    if(audioReactiveControlSwitch==1){
                        midiMidActiveFloat[10]=0;
                        midiHighActiveFloat[10]=0;
                        if(forceMidiWrite || abs((message.value-MIDI_MAGIC)/MIDI_MAGIC-lowC11)<CONTROL_THRESHOLD){
                            midiLowActiveFloat[10]=1;
                        }
                        if(midiLowActiveFloat[10]==1){
                            lowC11=(message.value-MIDI_MAGIC)/MIDI_MAGIC;
                            if(z_2==true){ lowC11=2.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(z_5==true){ lowC11=5.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(z_10==true){ lowC11=10.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                        }
                    }
                    if(audioReactiveControlSwitch==2){
                        midiLowActiveFloat[10]=0;
                        midiHighActiveFloat[10]=0;
                        if(forceMidiWrite || abs((message.value-MIDI_MAGIC)/MIDI_MAGIC-midC11)<CONTROL_THRESHOLD){
                            midiMidActiveFloat[10]=1;
                        }
                        if(midiMidActiveFloat[10]==1){
                            midC11=(message.value-MIDI_MAGIC)/MIDI_MAGIC;
                            if(z_2==true){ midC11=2.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(z_5==true){ midC11=5.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(z_10==true){ midC11=10.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                        }
                    }
                    if(audioReactiveControlSwitch==3){
                        midiLowActiveFloat[10]=0;
                        midiMidActiveFloat[10]=0;
                        if(forceMidiWrite || abs((message.value-MIDI_MAGIC)/MIDI_MAGIC-highC11)<CONTROL_THRESHOLD){
                            midiHighActiveFloat[10]=1;
                        }
                        if(midiHighActiveFloat[10]==1){
                            highC11=(message.value-MIDI_MAGIC)/MIDI_MAGIC;
                            if(z_2==true){ highC11=2.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(z_5==true){ highC11=5.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(z_10==true){ highC11=10.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                        }
                    }
                }

                // CC123 — rotation (bipolar)
                if(message.control==123){
                    if(audioReactiveControlSwitch==0){
                        midiLowActiveFloat[11]=0;
                        midiMidActiveFloat[11]=0;
                        midiHighActiveFloat[11]=0;
                        {
                            vmidiActiveFloat[11]=0;
                            { float rRaw=(message.value-MIDI_MAGIC)/MIDI_MAGIC; float rMul=(r_2?2.0f:r_5?5.0f:r_10?10.0f:1.0f);
                            if(forceMidiWrite || abs(rRaw*rMul-manualControlTargets[11])<CONTROL_THRESHOLD*rMul){ midiActiveFloat[11]=1; }
                            if(midiActiveFloat[11]==1){ manualControlTargets[11]=rRaw*rMul; } }
                        }
                        if(videoReactiveSwitch==1){
                            midiActiveFloat[11]=0;
                            { float rRaw=(message.value-MIDI_MAGIC)/MIDI_MAGIC; float rMul=(r_2?2.0f:r_5?5.0f:r_10?10.0f:1.0f);
                            if(forceMidiWrite || abs(rRaw*rMul-vRotate)<CONTROL_THRESHOLD*rMul){ vmidiActiveFloat[11]=1; }
                            if(vmidiActiveFloat[11]==1){ vRotate=rRaw*rMul; } }
                        }
                    }
                    if(audioReactiveControlSwitch!=0){
                        midiActiveFloat[11]=0;
                        vmidiActiveFloat[11]=0;
                    }
                    if(audioReactiveControlSwitch==1){
                        midiMidActiveFloat[11]=0;
                        midiHighActiveFloat[11]=0;
                        if(forceMidiWrite || abs((message.value-MIDI_MAGIC)/MIDI_MAGIC-lowC12)<CONTROL_THRESHOLD){
                            midiLowActiveFloat[11]=1;
                        }
                        if(midiLowActiveFloat[11]==1){
                            lowC12=(message.value-MIDI_MAGIC)/MIDI_MAGIC;
                            if(r_2==true){ lowC12=2.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(r_5==true){ lowC12=5.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(r_10==true){ lowC12=10.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                        }
                    }
                    if(audioReactiveControlSwitch==2){
                        midiLowActiveFloat[11]=0;
                        midiHighActiveFloat[11]=0;
                        if(forceMidiWrite || abs((message.value-MIDI_MAGIC)/MIDI_MAGIC-midC12)<CONTROL_THRESHOLD){
                            midiMidActiveFloat[11]=1;
                        }
                        if(midiMidActiveFloat[11]==1){
                            midC12=(message.value-MIDI_MAGIC)/MIDI_MAGIC;
                            if(r_2==true){ midC12=2.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(r_5==true){ midC12=5.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(r_10==true){ midC12=10.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                        }
                    }
                    if(audioReactiveControlSwitch==3){
                        midiLowActiveFloat[11]=0;
                        midiMidActiveFloat[11]=0;
                        if(forceMidiWrite || abs((message.value-MIDI_MAGIC)/MIDI_MAGIC-highC12)<CONTROL_THRESHOLD){
                            midiHighActiveFloat[11]=1;
                        }
                        if(midiHighActiveFloat[11]==1){
                            highC12=(message.value-MIDI_MAGIC)/MIDI_MAGIC;
                            if(r_2==true){ highC12=2.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(r_5==true){ highC12=5.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(r_10==true){ highC12=10.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                        }
                    }
                }

                // CC124 — hue modulo (unipolar)
                if(message.control==124){
                    if(audioReactiveControlSwitch==0){
                        midiLowActiveFloat[12]=0;
                        midiMidActiveFloat[12]=0;
                        midiHighActiveFloat[12]=0;
                        {
                            vmidiActiveFloat[12]=0;
                            { float hxxDiv=(huexx_0?64.0f:huexx_1?96.0f:huexx_2?127.0f:32.0f); float hxxVal=message.value/hxxDiv;
                            if(forceMidiWrite || abs(hxxVal-manualControlTargets[12])<CONTROL_THRESHOLD*127.0f/hxxDiv){ midiActiveFloat[12]=1; }
                            if(midiActiveFloat[12]==1){ manualControlTargets[12]=hxxVal; } }
                        }
                        if(videoReactiveSwitch==1){
                            midiActiveFloat[12]=0;
                            { float hxxDiv=(huexx_0?64.0f:huexx_1?96.0f:huexx_2?127.0f:32.0f); float hxxVal=message.value/hxxDiv;
                            if(forceMidiWrite || abs(hxxVal-vHuexMod)<CONTROL_THRESHOLD*127.0f/hxxDiv){ vmidiActiveFloat[12]=1; }
                            if(vmidiActiveFloat[12]==1){ vHuexMod=hxxVal; } }
                        }
                    }
                    if(audioReactiveControlSwitch!=0){
                        midiActiveFloat[12]=0;
                        vmidiActiveFloat[12]=0;
                    }
                    if(audioReactiveControlSwitch==1){
                        midiMidActiveFloat[12]=0;
                        midiHighActiveFloat[12]=0;
                        if(forceMidiWrite || abs(message.value/127.0f-lowC13)<CONTROL_THRESHOLD){
                            midiLowActiveFloat[12]=1;
                        }
                        if(midiLowActiveFloat[12]==1){
                            lowC13=(message.value)/32.0f;
                            if(huexx_0==true){ lowC13=message.value/64.0f; }
                            if(huexx_1==true){ lowC13=message.value/96.0f; }
                            if(huexx_2==true){ lowC13=message.value/127.0f; }
                        }
                    }
                    if(audioReactiveControlSwitch==2){
                        midiLowActiveFloat[12]=0;
                        midiHighActiveFloat[12]=0;
                        if(forceMidiWrite || abs(message.value/127.0f-midC13)<CONTROL_THRESHOLD){
                            midiMidActiveFloat[12]=1;
                        }
                        if(midiMidActiveFloat[12]==1){
                            midC13=(message.value)/32.0f;
                            if(huexx_0==true){ midC13=message.value/64.0f; }
                            if(huexx_1==true){ midC13=message.value/96.0f; }
                            if(huexx_2==true){ midC13=message.value/127.0f; }
                        }
                    }
                    if(audioReactiveControlSwitch==3){
                        midiLowActiveFloat[12]=0;
                        midiMidActiveFloat[12]=0;
                        if(forceMidiWrite || abs(message.value/127.0f-highC13)<CONTROL_THRESHOLD){
                            midiHighActiveFloat[12]=1;
                        }
                        if(midiHighActiveFloat[12]==1){
                            highC13=(message.value)/32.0f;
                            if(huexx_0==true){ highC13=message.value/64.0f; }
                            if(huexx_1==true){ highC13=message.value/96.0f; }
                            if(huexx_2==true){ highC13=message.value/127.0f; }
                        }
                    }
                }

                // CC125 — hue offset (bipolar)
                if(message.control==125){
                    if(audioReactiveControlSwitch==0){
                        midiLowActiveFloat[13]=0;
                        midiMidActiveFloat[13]=0;
                        midiHighActiveFloat[13]=0;
                        {
                            vmidiActiveFloat[13]=0;
                            { float hxyRaw=(message.value-MIDI_MAGIC)/MIDI_MAGIC; float hxyMul=(huexy_0?2.0f:huexy_1?4.0f:huexy_2?8.0f:1.0f);
                            if(forceMidiWrite || abs(hxyRaw*hxyMul-manualControlTargets[13])<CONTROL_THRESHOLD*hxyMul){ midiActiveFloat[13]=1; }
                            if(midiActiveFloat[13]==1){ manualControlTargets[13]=hxyRaw*hxyMul; } }
                        }
                        if(videoReactiveSwitch==1){
                            midiActiveFloat[13]=0;
                            { float hxyRaw=(message.value-MIDI_MAGIC)/MIDI_MAGIC; float hxyMul=(huexy_0?2.0f:huexy_1?4.0f:huexy_2?8.0f:1.0f);
                            if(forceMidiWrite || abs(hxyRaw*hxyMul-vHuexOff)<CONTROL_THRESHOLD*hxyMul){ vmidiActiveFloat[13]=1; }
                            if(vmidiActiveFloat[13]==1){ vHuexOff=hxyRaw*hxyMul; } }
                        }
                    }
                    if(audioReactiveControlSwitch!=0){
                        midiActiveFloat[13]=0;
                        vmidiActiveFloat[13]=0;
                    }
                    if(audioReactiveControlSwitch==1){
                        midiMidActiveFloat[13]=0;
                        midiHighActiveFloat[13]=0;
                        if(forceMidiWrite || abs((message.value-MIDI_MAGIC)/MIDI_MAGIC-lowC14)<CONTROL_THRESHOLD){
                            midiLowActiveFloat[13]=1;
                        }
                        if(midiLowActiveFloat[13]==1){
                            lowC14=(message.value-MIDI_MAGIC)/MIDI_MAGIC;
                            if(huexy_0==true){ lowC14=2.0f*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(huexy_1==true){ lowC14=4.0f*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(huexy_2==true){ lowC14=8.0f*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                        }
                    }
                    if(audioReactiveControlSwitch==2){
                        midiLowActiveFloat[13]=0;
                        midiHighActiveFloat[13]=0;
                        if(forceMidiWrite || abs((message.value-MIDI_MAGIC)/MIDI_MAGIC-midC14)<CONTROL_THRESHOLD){
                            midiMidActiveFloat[13]=1;
                        }
                        if(midiMidActiveFloat[13]==1){
                            midC14=(message.value-MIDI_MAGIC)/MIDI_MAGIC;
                            if(huexy_0==true){ midC14=2.0f*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(huexy_1==true){ midC14=4.0f*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(huexy_2==true){ midC14=8.0f*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                        }
                    }
                    if(audioReactiveControlSwitch==3){
                        midiLowActiveFloat[13]=0;
                        midiMidActiveFloat[13]=0;
                        if(forceMidiWrite || abs((message.value-MIDI_MAGIC)/MIDI_MAGIC-highC14)<CONTROL_THRESHOLD){
                            midiHighActiveFloat[13]=1;
                        }
                        if(midiHighActiveFloat[13]==1){
                            highC14=(message.value-MIDI_MAGIC)/MIDI_MAGIC;
                            if(huexy_0==true){ highC14=2.0f*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(huexy_1==true){ highC14=4.0f*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(huexy_2==true){ highC14=8.0f*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                        }
                    }
                }

                // CC126 — hue lfo (bipolar)
                if(message.control==126){
                    if(audioReactiveControlSwitch==0){
                        midiLowActiveFloat[14]=0;
                        midiMidActiveFloat[14]=0;
                        midiHighActiveFloat[14]=0;
                        {
                            vmidiActiveFloat[14]=0;
                            { float hxzRaw=(message.value-MIDI_MAGIC)/MIDI_MAGIC; float hxzMul=(huexz_0?2.0f:huexz_1?4.0f:huexz_2?8.0f:1.0f);
                            if(forceMidiWrite || abs(hxzRaw*hxzMul-manualControlTargets[14])<CONTROL_THRESHOLD*hxzMul){ midiActiveFloat[14]=1; }
                            if(midiActiveFloat[14]==1){ manualControlTargets[14]=hxzRaw*hxzMul; } }
                        }
                        if(videoReactiveSwitch==1){
                            midiActiveFloat[14]=0;
                            { float hxzRaw=(message.value-MIDI_MAGIC)/MIDI_MAGIC; float hxzMul=(huexz_0?2.0f:huexz_1?4.0f:huexz_2?8.0f:1.0f);
                            if(forceMidiWrite || abs(hxzRaw*hxzMul-vHuexLfo)<CONTROL_THRESHOLD*hxzMul){ vmidiActiveFloat[14]=1; }
                            if(vmidiActiveFloat[14]==1){ vHuexLfo=hxzRaw*hxzMul; } }
                        }
                    }
                    if(audioReactiveControlSwitch!=0){
                        midiActiveFloat[14]=0;
                        vmidiActiveFloat[14]=0;
                    }
                    if(audioReactiveControlSwitch==1){
                        midiMidActiveFloat[14]=0;
                        midiHighActiveFloat[14]=0;
                        if(forceMidiWrite || abs((message.value-MIDI_MAGIC)/MIDI_MAGIC-lowC15)<CONTROL_THRESHOLD){
                            midiLowActiveFloat[14]=1;
                        }
                        if(midiLowActiveFloat[14]==1){
                            lowC15=(message.value-MIDI_MAGIC)/MIDI_MAGIC;
                            if(huexz_0==true){ lowC15=2.0f*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(huexz_1==true){ lowC15=4.0f*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(huexz_2==true){ lowC15=8.0f*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                        }
                    }
                    if(audioReactiveControlSwitch==2){
                        midiLowActiveFloat[14]=0;
                        midiHighActiveFloat[14]=0;
                        if(forceMidiWrite || abs((message.value-MIDI_MAGIC)/MIDI_MAGIC-midC15)<CONTROL_THRESHOLD){
                            midiMidActiveFloat[14]=1;
                        }
                        if(midiMidActiveFloat[14]==1){
                            midC15=(message.value-MIDI_MAGIC)/MIDI_MAGIC;
                            if(huexz_0==true){ midC15=2.0f*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(huexz_1==true){ midC15=4.0f*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(huexz_2==true){ midC15=8.0f*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                        }
                    }
                    if(audioReactiveControlSwitch==3){
                        midiLowActiveFloat[14]=0;
                        midiMidActiveFloat[14]=0;
                        if(forceMidiWrite || abs((message.value-MIDI_MAGIC)/MIDI_MAGIC-highC15)<CONTROL_THRESHOLD){
                            midiHighActiveFloat[14]=1;
                        }
                        if(midiHighActiveFloat[14]==1){
                            highC15=(message.value-MIDI_MAGIC)/MIDI_MAGIC;
                            if(huexz_0==true){ highC15=2.0f*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(huexz_1==true){ highC15=4.0f*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(huexz_2==true){ highC15=8.0f*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                        }
                    }
                }

                // CC127 — delay (unipolar, no video reactive)
                if(message.control==127){
                    if(audioReactiveControlSwitch==0){
                        midiLowActiveFloat[15]=0;
                        midiMidActiveFloat[15]=0;
                        midiHighActiveFloat[15]=0;
                        {
                            if(forceMidiWrite || abs(message.value/127.0f-manualControlTargets[15])<CONTROL_THRESHOLD){
                                midiActiveFloat[15]=1;
                            }
                            if(midiActiveFloat[15]==1){
                                manualControlTargets[15]=(message.value)/127.0;
                            }
                        }
                    }
                    if(audioReactiveControlSwitch!=0){
                        midiActiveFloat[15]=0;
                        vmidiActiveFloat[15]=0;
                    }
                    if(audioReactiveControlSwitch==1){
                        midiMidActiveFloat[15]=0;
                        midiHighActiveFloat[15]=0;
                        if(forceMidiWrite || abs(message.value/127.0f-lowC16)<CONTROL_THRESHOLD){
                            midiLowActiveFloat[15]=1;
                        }
                        if(midiLowActiveFloat[15]==1){ lowC16=message.value/127.0f; }
                    }
                    if(audioReactiveControlSwitch==2){
                        midiLowActiveFloat[15]=0;
                        midiHighActiveFloat[15]=0;
                        if(forceMidiWrite || abs(message.value/127.0f-midC16)<CONTROL_THRESHOLD){
                            midiMidActiveFloat[15]=1;
                        }
                        if(midiMidActiveFloat[15]==1){ midC16=message.value/127.0f; }
                    }
                    if(audioReactiveControlSwitch==3){
                        midiLowActiveFloat[15]=0;
                        midiMidActiveFloat[15]=0;
                        if(forceMidiWrite || abs(message.value/127.0f-highC16)<CONTROL_THRESHOLD){
                            midiHighActiveFloat[15]=1;
                        }
                        if(midiHighActiveFloat[15]==1){ highC16=message.value/127.0f; }
                    }
                }

                // ============================================================
                // BOOLEAN CONTROLS
                // ============================================================
                if(message.control==60){
                    if(message.value==127){ brightInvert=true; }
                    if(message.value==0){ brightInvert=false; }
                }
                if(message.control==61){
                    if(message.value==127){ saturationInvert=true; }
                    if(message.value==0){ saturationInvert=false; }
                }
                if(message.control==62){
                    if(message.value==127){ hueInvert=true; }
                    if(message.value==0){ hueInvert=false; }
                }
                if(message.control==90){
                    if(message.value==127){ hdmi_aspect_ratio_switch=true; }
                    if(message.value==0){ hdmi_aspect_ratio_switch=false; }
                }
                if(message.control==41){
                    if(message.value==127){ horizontalMirror=true; }
                    if(message.value==0){ horizontalMirror=false; }
                }
                if(message.control==45){
                    if(message.value==127){ verticalMirror=true; }
                    if(message.value==0){ verticalMirror=false; }
                }
                if(message.control==59){
                    if(message.value==127){
                        vLumakeyValue=0.0; vMix=0.0; vHue=0.0; vSaturation=0.0;
                        vBright=0.0; vTemporalFilterMix=0.0; vTemporalFilterResonance=0.0;
                        vSharpenAmount=0.0; vX=0.0; vY=0.0; vZ=0.0; vRotate=0.0;
                        vHuexMod=0.0; vHuexOff=0.0; vHuexLfo=0.0;
                        lowC1=lowC2=lowC3=lowC4=lowC5=lowC6=lowC7=lowC8=lowC9=lowC10=lowC11=lowC12=lowC13=lowC14=lowC15=lowC16=lowC17=lowC18=lowC19=lowC20=0.0f;
                        midC1=midC2=midC3=midC4=midC5=midC6=midC7=midC8=midC9=midC10=midC11=midC12=midC13=midC14=midC15=midC16=midC17=midC18=midC19=midC20=0.0f;
                        highC1=highC2=highC3=highC4=highC5=highC6=highC7=highC8=highC9=highC10=highC11=highC12=highC13=highC14=highC15=highC16=highC17=highC18=highC19=highC20=0.0f;
                        for(int i=0;i<manualControlCount;i++){
                            midiActiveFloat[i]=0; vmidiActiveFloat[i]=0;
                            midiLowActiveFloat[i]=0; midiMidActiveFloat[i]=0; midiHighActiveFloat[i]=0;
                            manualControlTargets[i]=0;
                        }
                    }
                }
                if(message.control==58){
                    if(message.value==127){
                        if(clear_switch==0){
                            clear_switch=1;
                            framebuffer0.begin();
    // DEBUG: print texture dimensions once
    static bool printed = false;
    if(!printed && cam1.isInitialized()) {
        ofLogNotice() << "TEX_DEBUG cam1=" << cam1.getTexture().getWidth() << "x" << cam1.getTexture().getHeight()
                      << " fb0=" << framebuffer0.getWidth() << "x" << framebuffer0.getHeight();
        printed = true;
    } ofClear(0,0,0,255); framebuffer0.end();
                            for(int i=0;i<framebufferLength;i++){
                                pastFrames[i].begin(); ofClear(0,0,0,255); pastFrames[i].end();
                            }
                        }
                    }
                }
                if(message.control!=58){ clear_switch=0; }
            }
        }
    }
    midiPreserveRepeatedControls.store(x_skew_switch || y_skew_switch);
    if(actionOverflow) {
        forceMidiWrite=false;forceMidiWriteTimer=0;
        audioReactiveControlSwitch=0;videoReactiveSwitch=0;midiLatchClear();
        ofLogError()<<"MIDI_QUEUE action-capacity overflow; assignment/latch state cleared; reassert controller state";
    }

}
//-----------------------------------------------------------
void ofApp::midibizOld(){
    for(unsigned int i = 0; i < midiMessages.size(); ++i) {
        
        ofxMidiMessage &message = midiMessages[i];
        
        if(message.status < MIDI_SYSEX) {
            //text << "chan: " << message.channel;
            if(message.status == MIDI_CONTROL_CHANGE) {
                
 
               //How to Midi Map
                //uncomment the line that says cout<<message control etc
                //run the code and look down in the console
                //when u move a knob on yr controller keep track of the number that shows up
                //that is the cc value of the knob
                //then go down to the part labled 'MIDIMAPZONE'
                //and change the numbers for each if message.control== statement to the values
                //on yr controller
                
                // cout << "message.control"<< message.control<< endl;
                // cout << "message.value"<< message.value<< endl;
                
                
                
                //MIDIMAPZONE
                //these are mostly all set to output bipolor controls at this moment (ranging from -1.0 to 1.0)
                //if u uncomment the second line on each of these if statements that will switch thems to unipolor
                //controls (ranging from 0.0to 1.0) if  you prefer
                
                if(message.control==39){
                    if(message.value==127){
                        legacyManualControlsEnabled=0;
                    }
                    if(message.value==0){
                        legacyManualControlsEnabled=1;
                    }
                }
                
                if(message.control==58){
                    if(message.value==127){
                        for(int i=0;i<manualControlCount;i++){
                            manualControlTargets[i]=0;
                        }
                    }
                }
                if(message.control==32){
                    if(message.value==127){
                        x_2=true;
                    }
                    if(message.value==0){
                        x_2=false;
                    }
                }
                if(message.control==48){
                    if(message.value==127){
                        x_5=true;
                    }
                    if(message.value==0){
                        x_5=false;
                    }
                }
                if(message.control==64){
                    if(message.value==127){
                        x_10=true;
                    }
                    
                    if(message.value==0){
                        x_10=false;
                    }
                }
                
                
                
                if(message.control==33){
                    if(message.value==127){
                        y_2=true;
                    }
                    
                    if(message.value==0){
                        y_2=false;
                    }
                }
                
                if(message.control==49){
                    if(message.value==127){
                        y_5=true;
                    }
                    
                    if(message.value==0){
                        y_5=false;
                    }
                }
                
                if(message.control==65){
                    if(message.value==127){
                        y_10=true;
                    }
                    
                    if(message.value==0){
                        y_10=false;
                    }
                }
                
                
                
                if(message.control==34){
                    if(message.value==127){
                        z_2=true;
                    }
                    
                    if(message.value==0){
                        z_2=false;
                    }
                }
                
                if(message.control==50){
                    if(message.value==127){
                        z_5=true;
                    }
                    
                    if(message.value==0){
                        z_5=false;
                    }
                }
                
                if(message.control==66){
                    if(message.value==127){
                        z_10=true;
                    }
                    
                    if(message.value==0){
                        z_10=false;
                    }
                }
                
                
                
                
                if(message.control==35){
                    if(message.value==127){
                        r_2=true;
                    }
                    
                    if(message.value==0){
                        r_2=false;
                    }
                }
                
                if(message.control==51){
                    if(message.value==127){
                        r_5=true;
                    }
                    
                    if(message.value==0){
                        r_5=false;
                    }
                }
                
                if(message.control==67){
                    if(message.value==127){
                        r_10=true;
                    }
                    
                    if(message.value==0){
                        r_10=false;
                    }
                }
                
                
                
                
                if(message.control==36){
                    if(message.value==127){
                        huexx_0=true;
                    }
                    
                    if(message.value==0){
                        huexx_0=false;
                    }
                }
                
                if(message.control==52){
                    if(message.value==127){
                        huexx_1=true;
                    }
                    
                    if(message.value==0){
                        huexx_1=false;
                    }
                }
                
                if(message.control==68){
                    if(message.value==127){
                        huexx_2=true;
                    }
                    
                    if(message.value==0){
                        huexx_2=false;
                    }
                }
                
                if(message.control==46){
                    if(message.value==127){
                        toroidSwitch=true;
                    }
                    
                    if(message.value==0){
                        toroidSwitch=false;
                    }
                }
                
                
                
                
                if(y_skew_switch==true){
                    y_skew+=.00001;
                }
                
                if(x_skew_switch==true){
                    x_skew+=.00001;
                }
                
                
                if(message.control==71){
                    if(message.value==127){
                        wet_dry_switch=false;
                    }
                    
                    if(message.value==0){
                        wet_dry_switch=true;
                    }
                    
                }
                
                //---------------------
                
                
                
                if(message.control==37){
                    if(message.value==127){
                        huexy_0=true;
                    }
                    
                    if(message.value==0){
                        huexy_0=false;
                    }
                }
                
                if(message.control==53){
                    if(message.value==127){
                        huexy_1=true;
                    }
                    
                    if(message.value==0){
                        huexy_1=false;
                    }
                }
                
                if(message.control==69){
                    if(message.value==127){
                        huexy_2=true;
                    }
                    
                    if(message.value==0){
                        huexy_2=false;
                    }
                }
                
                
                //---------------------
                
                
                
                if(message.control==38){
                    if(message.value==127){
                        huexz_0=true;
                    }
                    
                    if(message.value==0){
                        huexz_0=false;
                    }
                }
                
                if(message.control==54){
                    if(message.value==127){
                        huexz_1=true;
                    }
                    
                    if(message.value==0){
                        huexz_1=false;
                    }
                }
                
                if(message.control==70){
                    if(message.value==127){
                        huexz_2=true;
                    }
                    
                    if(message.value==0){
                        huexz_2=false;
                    }
                }
                if(message.control==43){
                    if(message.value==127){
                        audioReactiveControlSwitch=1;
                    }
                    
                    if(message.value==0){
                        audioReactiveControlSwitch=0;
                    }
                    
                }
                
                if(message.control==44){
                    if(message.value==127){
                        audioReactiveControlSwitch=2;
                    }
                    
                    if(message.value==0){
                        audioReactiveControlSwitch=0;
                    }
                    
                }
                
                if(message.control==42){
                    if(message.value==127){
                        audioReactiveControlSwitch=3;
                    }
                    
                    if(message.value==0){
                        audioReactiveControlSwitch=0;
                    }
                    
                }
                //nanokontrol2 controls
                //c1 maps to fb0 lumakey
                if(message.control==16){
                    
                    if(audioReactiveControlSwitch==0){
                        if(legacyManualControlsEnabled==1){
                            manualControlTargets[0]=message.value/127.0f;
                        }
                    }
                    
                    if(audioReactiveControlSwitch==1){
                        lowC1=message.value/127.0f;
                    }
                    
                    if(audioReactiveControlSwitch==2){
                        midC1=message.value/127.0f;
                    }
                    
                    if(audioReactiveControlSwitch==3){
                        highC1=message.value/127.0f;
                    }
                }
                
                if(message.control==17){
                    
                    if(audioReactiveControlSwitch==0){
                        if(legacyManualControlsEnabled==1){
                            manualControlTargets[1]=(message.value-63)/63.0f;
                        }
                    }
                    
                    if(audioReactiveControlSwitch==1){
                        lowC2=(message.value-63.0f)/63.0f;
                    }
                    
                    if(audioReactiveControlSwitch==2){
                        midC2=(message.value-63.0f)/63.0f;
                    }
                    
                    if(audioReactiveControlSwitch==3){
                        highC2=(message.value-63.0f)/63.0f;
                    }
                }
                
                //c3 maps to fb0 huex
                if(message.control==18){
                    if(audioReactiveControlSwitch==0){
                        if(legacyManualControlsEnabled==1){
                            manualControlTargets[2]=(message.value-63)/63.0f;
                        }
                    }
                    if(audioReactiveControlSwitch==1){
                        lowC3=(message.value-63.0f)/63.0f;
                    }
                    
                    if(audioReactiveControlSwitch==2){
                        midC3=(message.value-63.0f)/63.0f;
                    }
                    
                    if(audioReactiveControlSwitch==3){
                        highC3=(message.value-63.0f)/63.0f;
                    }
                    
                }
                
                //c4 maps to fb0 satx
                if(message.control==19){
                    if(audioReactiveControlSwitch==0){
                        if(legacyManualControlsEnabled==1){
                            manualControlTargets[3]=(message.value-63)/63.0f;
                        }
                    }
                    if(audioReactiveControlSwitch==1){
                        lowC4=(message.value-63.0f)/63.0f;
                    }
                    
                    if(audioReactiveControlSwitch==2){
                        midC4=(message.value-63.0f)/63.0f;
                    }
                    
                    if(audioReactiveControlSwitch==3){
                        highC4=(message.value-63.0f)/63.0f;
                    }
                }
                
                //c5 maps to fb0 brightx
                if(message.control==20){
                    if(audioReactiveControlSwitch==0){
                        if(legacyManualControlsEnabled==1){
                            manualControlTargets[4]=(message.value-63)/63.0f;
                        }
                    }
                    if(audioReactiveControlSwitch==1){
                        lowC5=(message.value-63.0f)/63.0f;
                    }
                    
                    if(audioReactiveControlSwitch==2){
                        midC5=(message.value-63.0f)/63.0f;
                    }
                    
                    if(audioReactiveControlSwitch==3){
                        highC5=(message.value-63.0f)/63.0f;
                    }
                    
                }
                
                //c6 maps to temporal filter
                if(message.control==21){
                    if(audioReactiveControlSwitch==0){
                        if(legacyManualControlsEnabled==1){
                            manualControlTargets[5]=(message.value-63)/63.0f;
                        }
                    }
                    if(audioReactiveControlSwitch==1){
                        lowC6=(message.value-63.0f)/63.0f;
                    }
                    
                    if(audioReactiveControlSwitch==2){
                        midC6=(message.value-63.0f)/63.0f;
                    }
                    
                    if(audioReactiveControlSwitch==3){
                        highC6=(message.value-63.0f)/63.0f;
                    }
                }
                
                //c7 maps to temporal filter resonance
                if(message.control==22){
                    if(audioReactiveControlSwitch==0){
                        if(legacyManualControlsEnabled==1){
                            manualControlTargets[6]=(message.value)/127.0f;
                        }
                    }
                    if(audioReactiveControlSwitch==1){
                        lowC7=(message.value)/127.0f;
                    }
                    
                    if(audioReactiveControlSwitch==2){
                        midC7=(message.value)/127.0f;
                    }
                    
                    if(audioReactiveControlSwitch==3){
                        highC7=(message.value)/127.0f;
                    }
                }
                
                //c8 maps to brightx
                if(message.control==23){
                    if(audioReactiveControlSwitch==0){
                        if(legacyManualControlsEnabled==1){
                            manualControlTargets[7]=(message.value)/127.0f;
                        }
                    }
                    if(audioReactiveControlSwitch==1){
                        lowC8=(message.value)/127.0f;
                    }
                    
                    if(audioReactiveControlSwitch==2){
                        midC8=(message.value)/127.0f;
                    }
                    
                    if(audioReactiveControlSwitch==3){
                        highC8=(message.value)/127.0f;
                    }
                    
                }
                
                //c9 maps to fb0 x displace
                if(message.control==120){
                    if(audioReactiveControlSwitch==0){
                        manualControlTargets[8]=(message.value-63)/63.0f;
                        
                        if(x_2==true){
                            manualControlTargets[8]=2.0*(message.value-63.0)/63.0f;
                        }
                        
                        if(x_5==true){
                            manualControlTargets[8]=5.0*(message.value-63.0)/63.0f;
                        }
                        if(x_10==true){
                            manualControlTargets[8]=10.0*(message.value-63.0)/63.0f;
                        }
                    }
                    
                    if(audioReactiveControlSwitch==1){
                        lowC9=(message.value-63)/63.0f;
                        
                        if(x_2==true){
                            lowC9=2.0*(message.value-63.0)/63.0f;
                        }
                        
                        if(x_5==true){
                            lowC9=5.0*(message.value-63.0)/63.0f;
                        }
                        if(x_10==true){
                            lowC9=10.0*(message.value-63.0)/63.0f;
                        }
                    }
                    
                    if(audioReactiveControlSwitch==2){
                        midC9=(message.value-63)/63.0f;
                        
                        if(x_2==true){
                            midC9=2.0*(message.value-63.0)/63.0f;
                        }
                        
                        if(x_5==true){
                            midC9=5.0*(message.value-63.0)/63.0f;
                        }
                        if(x_10==true){
                            midC9=10.0*(message.value-63.0)/63.0f;
                        }
                    }
                    
                    if(audioReactiveControlSwitch==3){
                        highC9=(message.value-63)/63.0f;
                        
                        if(x_2==true){
                            highC9=2.0*(message.value-63.0)/63.0f;
                        }
                        
                        if(x_5==true){
                            highC9=5.0*(message.value-63.0)/63.0f;
                        }
                        if(x_10==true){
                            highC9=10.0*(message.value-63.0)/63.0f;
                        }
                    }
                    
                }
                
                //c10 maps to fb0 y displace
                if(message.control==121){
                    if(audioReactiveControlSwitch==0){
                        manualControlTargets[9]=(message.value-63)/63.0f;
                        
                        if(y_2==true){
                            manualControlTargets[9]=2.0*(message.value-63.0)/63.0f;
                        }
                        
                        if(y_5==true){
                            manualControlTargets[9]=5.0*(message.value-63.0)/63.0f;
                        }
                        if(y_10==true){
                            manualControlTargets[9]=10.0*(message.value-63.0)/63.0f;
                        }
                    }
                    
                    if(audioReactiveControlSwitch==1){
                        lowC10=(message.value-63)/63.0f;
                        
                        if(y_2==true){
                            lowC10=2.0*(message.value-63.0)/63.0f;
                        }
                        
                        if(y_5==true){
                            lowC10=5.0*(message.value-63.0)/63.0f;
                        }
                        if(y_10==true){
                            lowC10=10.0*(message.value-63.0)/63.0f;
                        }
                    }
                    
                    if(audioReactiveControlSwitch==2){
                        midC10=(message.value-63)/63.0f;
                        
                        if(y_2==true){
                            midC10=2.0*(message.value-63.0)/63.0f;
                        }
                        
                        if(y_5==true){
                            midC10=5.0*(message.value-63.0)/63.0f;
                        }
                        if(y_10==true){
                            midC10=10.0*(message.value-63.0)/63.0f;
                        }
                    }
                    
                    if(audioReactiveControlSwitch==3){
                        highC10=(message.value-63)/63.0f;
                        
                        if(y_2==true){
                            highC10=2.0*(message.value-63.0)/63.0f;
                        }
                        
                        if(y_5==true){
                            highC10=5.0*(message.value-63.0)/63.0f;
                        }
                        if(y_10==true){
                            highC10=10.0*(message.value-63.0)/63.0f;
                        }
                    }
                    
                    
                    
                }
                
                
                if(message.control==122){
                    if(audioReactiveControlSwitch==0){
                        manualControlTargets[10]=(message.value-63.0)/63.0f;
                        
                        if(z_2==true){
                            manualControlTargets[10]=2.0*(message.value-63.0)/63.0f;
                        }
                        
                        if(z_5==true){
                            manualControlTargets[10]=5.0*(message.value-63.0)/63.0f;
                        }
                        if(z_10==true){
                            manualControlTargets[10]=10.0*(message.value-63.0)/63.0f;
                        }
                    }
                    
                    if(audioReactiveControlSwitch==1){
                        lowC11=(message.value-63.0)/63.0f;
                        
                        if(z_2==true){
                            lowC11=2.0*(message.value-63.0)/63.0f;
                        }
                        
                        if(z_5==true){
                            lowC11=5.0*(message.value-63.0)/63.0f;
                        }
                        if(z_10==true){
                            lowC11=10.0*(message.value-63.0)/63.0f;
                        }
                    }
                    
                    if(audioReactiveControlSwitch==2){
                        midC11=(message.value-63.0)/63.0f;
                        
                        if(z_2==true){
                            midC11=2.0*(message.value-63.0)/63.0f;
                        }
                        
                        if(z_5==true){
                            midC11=5.0*(message.value-63.0)/63.0f;
                        }
                        if(z_10==true){
                            midC11=10.0*(message.value-63.0)/63.0f;
                        }
                    }
                    
                    if(audioReactiveControlSwitch==3){
                        highC11=(message.value-63.0)/63.0f;
                        
                        if(z_2==true){
                            highC11=2.0*(message.value-63.0)/63.0f;
                        }
                        
                        if(z_5==true){
                            highC11=5.0*(message.value-63.0)/63.0f;
                        }
                        if(z_10==true){
                            highC11=10.0*(message.value-63.0)/63.0f;
                        }
                    }
                    
                }
                
                if(message.control==123){
                    if(audioReactiveControlSwitch==0){
                        manualControlTargets[11]=(message.value-63)/63.0f;
                        
                        if(r_2==true){
                            manualControlTargets[11]=2*(message.value-63.0)/63.0f;
                        }
                        
                        if(r_5==true){
                            manualControlTargets[11]=4*(message.value-63.0)/63.0f;
                        }
                        if(r_10==true){
                            manualControlTargets[11]=8*(message.value-63.0)/63.0f;
                        }
                    }
                    
                    if(audioReactiveControlSwitch==1){
                        lowC12=(message.value-63)/63.0f;
                        
                        if(r_2==true){
                            lowC12=2*(message.value-63.0)/63.0f;
                        }
                        
                        if(r_5==true){
                            lowC12=4*(message.value-63.0)/63.0f;
                        }
                        if(r_10==true){
                            lowC12=8*(message.value-63.0)/63.0f;
                        }
                    }
                    
                    if(audioReactiveControlSwitch==2){
                        midC12=(message.value-63)/63.0f;
                        
                        if(r_2==true){
                            midC12=2*(message.value-63.0)/63.0f;
                        }
                        
                        if(r_5==true){
                            midC12=4*(message.value-63.0)/63.0f;
                        }
                        if(r_10==true){
                            midC12=8*(message.value-63.0)/63.0f;
                        }
                    }
                    
                    if(audioReactiveControlSwitch==3){
                        highC12=(message.value-63)/63.0f;
                        
                        if(r_2==true){
                            highC12=2*(message.value-63.0)/63.0f;
                        }
                        
                        if(r_5==true){
                            highC12=4*(message.value-63.0)/63.0f;
                        }
                        if(r_10==true){
                            highC12=8*(message.value-63.0)/63.0f;
                        }
                    }
                    
                    
                }
                
                
                if(message.control==124){
                    if(audioReactiveControlSwitch==0){
                        manualControlTargets[12]=(message.value)/32.0f;
                        
                        if(huexx_0==true){
                            manualControlTargets[12]=message.value/64.0f;
                        }
                        
                        if(huexx_1==true){
                            manualControlTargets[12]=message.value/96.0f;
                        }
                        if(huexx_2==true){
                            manualControlTargets[12]=message.value/127.0f;
                        }
                    }
                    
                    if(audioReactiveControlSwitch==1){
                        lowC13=(message.value)/32.0f;
                        
                        if(huexx_0==true){
                            lowC13=message.value/64.0f;
                        }
                        
                        if(huexx_1==true){
                            lowC13=message.value/96.0f;
                        }
                        if(huexx_2==true){
                            lowC13=message.value/127.0f;
                        }
                    }
                    
                    if(audioReactiveControlSwitch==2){
                        midC13=(message.value)/32.0f;
                        
                        if(huexx_0==true){
                            midC13=message.value/64.0f;
                        }
                        
                        if(huexx_1==true){
                            midC13=message.value/96.0f;
                        }
                        if(huexx_2==true){
                            midC13=message.value/127.0f;
                        }
                    }
                    
                    if(audioReactiveControlSwitch==3){
                        highC13=(message.value)/32.0f;
                        
                        if(huexx_0==true){
                            highC13=message.value/64.0f;
                        }
                        
                        if(huexx_1==true){
                            highC13=message.value/96.0f;
                        }
                        if(huexx_2==true){
                            highC13=message.value/127.0f;
                        }
                    }
                }
                
                
                if(message.control==125){
                    if(audioReactiveControlSwitch==0){
                        manualControlTargets[13]=(message.value-63.0)/63.0f;
                        
                        if(huexy_0==true){
                            manualControlTargets[13]=2*(message.value-63.0)/63.0f;
                        }
                        
                        if(huexy_1==true){
                            manualControlTargets[13]=4*(message.value-63.0)/63.0f;
                        }
                        if(huexy_2==true){
                            manualControlTargets[13]=8*(message.value-63.0)/63.0f;
                        }
                    }
                    
                    if(audioReactiveControlSwitch==1){
                        lowC14=(message.value-63.0)/63.0f;
                        
                        if(huexy_0==true){
                            lowC14=2*(message.value-63.0)/63.0f;
                        }
                        
                        if(huexy_1==true){
                            lowC14=4*(message.value-63.0)/63.0f;
                        }
                        if(huexy_2==true){
                            lowC14=8*(message.value-63.0)/63.0f;
                        }
                    }
                    
                    if(audioReactiveControlSwitch==2){
                        midC14=(message.value-63.0)/63.0f;
                        
                        if(huexy_0==true){
                            midC14=2*(message.value-63.0)/63.0f;
                        }
                        
                        if(huexy_1==true){
                            midC14=4*(message.value-63.0)/63.0f;
                        }
                        if(huexy_2==true){
                            midC14=8*(message.value-63.0)/63.0f;
                        }
                    }
                    
                    if(audioReactiveControlSwitch==3){
                        highC14=(message.value-63.0)/63.0f;
                        
                        if(huexy_0==true){
                            highC14=2*(message.value-63.0)/63.0f;
                        }
                        
                        if(huexy_1==true){
                            highC14=4*(message.value-63.0)/63.0f;
                        }
                        if(huexy_2==true){
                            highC14=8*(message.value-63.0)/63.0f;
                        }
                    }
                    
                    
                    
                    
                }
                
                
                if(message.control==126){
                    if(audioReactiveControlSwitch==0){
                        manualControlTargets[14]=(message.value-63.0)/63.0f;
                        
                        if(huexz_0==true){
                            manualControlTargets[14]=2*(message.value-63.0)/63.0f;
                        }
                        
                        if(huexz_1==true){
                            manualControlTargets[14]=4*(message.value-63.0)/63.0f;
                        }
                        if(huexz_2==true){
                            manualControlTargets[14]=8*(message.value-63.0)/63.0f;
                        }
                    }
                    
                    if(audioReactiveControlSwitch==1){
                        lowC15=(message.value-63.0)/63.0f;
                        
                        if(huexz_0==true){
                            lowC15=2*(message.value-63.0)/63.0f;
                        }
                        
                        if(huexz_1==true){
                            lowC15=4*(message.value-63.0)/63.0f;
                        }
                        if(huexz_2==true){
                            lowC15=8*(message.value-63.0)/63.0f;
                        }
                    }
                    
                    if(audioReactiveControlSwitch==2){
                        midC15=(message.value-63.0)/63.0f;
                        
                        if(huexz_0==true){
                            midC15=2*(message.value-63.0)/63.0f;
                        }
                        
                        if(huexz_1==true){
                            midC15=4*(message.value-63.0)/63.0f;
                        }
                        if(huexz_2==true){
                            midC15=8*(message.value-63.0)/63.0f;
                        }
                    }
                    
                    if(audioReactiveControlSwitch==3){
                        highC15=(message.value-63.0)/63.0f;
                        
                        if(huexz_0==true){
                            highC15=2*(message.value-63.0)/63.0f;
                        }
                        
                        if(huexz_1==true){
                            highC15=4*(message.value-63.0)/63.0f;
                        }
                        if(huexz_2==true){
                            highC15=8*(message.value-63.0)/63.0f;
                        }
                    }
                    
                    
                }
                
                
                if(message.control==127){
                    if(audioReactiveControlSwitch==0){
                        manualControlTargets[15]=(message.value)/127.0;
                    }
                    
                    if(audioReactiveControlSwitch==1){
                        lowC16=(message.value)/127.0;
                    }
                    
                    if(audioReactiveControlSwitch==2){
                        midC16=(message.value)/127.0;
                    }
                    
                    if(audioReactiveControlSwitch==3){
                        highC16=(message.value)/127.0;
                    }
                }
                
                
                
                
                //gots to remap these to 60-61
                //cc43 maps to brightInvert
                if(message.control==60){
                    if(message.value==127){
                        brightInvert=true;
                    }
                    
                    if(message.value==0){
                        brightInvert=false;
                    }
                }
                
                //cc44 maps to saturationInvert
                if(message.control==61){
                    if(message.value==127){
                        saturationInvert=true;
                    }
                    
                    if(message.value==0){
                        saturationInvert=false;
                    }
                }
                
                if(message.control==62){
                    if(message.value==127){
                        //hueInvert=true;
                        hdmi_aspect_ratio_switch=true;
                    }
                    
                    if(message.value==0){
                        //hueInvert=false;
                        hdmi_aspect_ratio_switch=false;
                    }
                }
                
                
                //cc41 maps to horizontalMirror
                if(message.control==41){
                    if(message.value==127){
                        horizontalMirror=true;
                    }
                    
                    if(message.value==0){
                        horizontalMirror=false;
                    }
                }
                //cc45 maps to horizontalMirror
                if(message.control==45){
                    if(message.value==127){
                        verticalMirror=true;
                    }
                    
                    if(message.value==0){
                        verticalMirror=false;
                    }
                }
                if(message.control==59){
                    
                    x_skew=y_skew=0.0;
                    lowC1=lowC2=lowC3=lowC4=lowC5=lowC6=lowC7=lowC8=lowC9=lowC10=lowC11=lowC12=lowC13=lowC14=lowC15=lowC16=0.0f;
                    midC1=midC2=midC3=midC4=midC5=midC6=midC7=midC8=midC9=midC10=midC11=midC12=midC13=midC14=midC15=midC16=0.0f;
                    highC1=highC2=highC3=highC4=highC5=highC6=highC7=highC8=highC9=highC10=highC11=highC12=highC13=highC14=highC15=highC16=0.0f;
                }
            }
        }
    }
}

//--------------------------------------------------------------
void ofApp::keyReleased(int key) {
}

std::string ofApp::audioPatchText(int slot){
    if(!autoeyez::validStoredAudioSlot(slot))throw std::runtime_error("audio slot");
    autoeyez::FftBands bands=autoeyez::FftBands::Contiguous;
    if(!autoeyez::selectedBandProfile(bands))throw std::runtime_error("band setting");
    std::ostringstream out;out.precision(9);out<<"audio_version=2\nslot="<<slot<<"\n";
    out<<"low1="<<lowC1<<"\n";
    out<<"low2="<<lowC2<<"\n";
    out<<"low3="<<lowC3<<"\n";
    out<<"low4="<<lowC4<<"\n";
    out<<"low5="<<lowC5<<"\n";
    out<<"low6="<<lowC6<<"\n";
    out<<"low7="<<lowC7<<"\n";
    out<<"low8="<<lowC8<<"\n";
    out<<"low9="<<lowC9<<"\n";
    out<<"low10="<<lowC10<<"\n";
    out<<"low11="<<lowC11<<"\n";
    out<<"low12="<<lowC12<<"\n";
    out<<"low13="<<lowC13<<"\n";
    out<<"low14="<<lowC14<<"\n";
    out<<"low15="<<lowC15<<"\n";
    out<<"low16="<<lowC16<<"\n";
    out<<"low17="<<lowC17<<"\n";
    out<<"low18="<<lowC18<<"\n";
    out<<"low19="<<lowC19<<"\n";
    out<<"low20="<<lowC20<<"\n";
    out<<"mid1="<<midC1<<"\n";
    out<<"mid2="<<midC2<<"\n";
    out<<"mid3="<<midC3<<"\n";
    out<<"mid4="<<midC4<<"\n";
    out<<"mid5="<<midC5<<"\n";
    out<<"mid6="<<midC6<<"\n";
    out<<"mid7="<<midC7<<"\n";
    out<<"mid8="<<midC8<<"\n";
    out<<"mid9="<<midC9<<"\n";
    out<<"mid10="<<midC10<<"\n";
    out<<"mid11="<<midC11<<"\n";
    out<<"mid12="<<midC12<<"\n";
    out<<"mid13="<<midC13<<"\n";
    out<<"mid14="<<midC14<<"\n";
    out<<"mid15="<<midC15<<"\n";
    out<<"mid16="<<midC16<<"\n";
    out<<"mid17="<<midC17<<"\n";
    out<<"mid18="<<midC18<<"\n";
    out<<"mid19="<<midC19<<"\n";
    out<<"mid20="<<midC20<<"\n";
    out<<"high1="<<highC1<<"\n";
    out<<"high2="<<highC2<<"\n";
    out<<"high3="<<highC3<<"\n";
    out<<"high4="<<highC4<<"\n";
    out<<"high5="<<highC5<<"\n";
    out<<"high6="<<highC6<<"\n";
    out<<"high7="<<highC7<<"\n";
    out<<"high8="<<highC8<<"\n";
    out<<"high9="<<highC9<<"\n";
    out<<"high10="<<highC10<<"\n";
    out<<"high11="<<highC11<<"\n";
    out<<"high12="<<highC12<<"\n";
    out<<"high13="<<highC13<<"\n";
    out<<"high14="<<highC14<<"\n";
    out<<"high15="<<highC15<<"\n";
    out<<"high16="<<highC16<<"\n";
    out<<"high17="<<highC17<<"\n";
    out<<"high18="<<highC18<<"\n";
    out<<"high19="<<highC19<<"\n";
    out<<"high20="<<highC20<<"\n";
    const char* coefficientBandNames[]={"low","mid","high"};
    for(int band=0;band<3;++band)
        for(int i=0;i<5;++i)
            out<<coefficientBandNames[band]<<(21+i)<<"="<<inputProcessingDepth[band][i]<<"\n";
    for(int band=0;band<3;++band)out<<coefficientBandNames[band]<<"26="<<holdDepth[band]<<"\n";
    out<<"react="<<audioReactiveControlSwitch<<"\n";
    autoeyez::writeEnvelopePatch(out,fftEnvelopeRuntime.envelope.config);autoeyez::writeBandPatch(out,bands);
    autoeyez::AudioPatch validated;if(!autoeyez::readAudioPatch(out.str(),slot,validated))throw std::runtime_error("invalid live audio state");
    return out.str();
}

void ofApp::patchTransactionPoll(){
    static autoeyez::FramePatchInbox inbox;
    auto writeState=[](const std::string& path,int value){const std::string tmp=path+".patch.tmp";std::ofstream file(tmp);file<<value;file.close();if(!file||std::rename(tmp.c_str(),path.c_str())!=0)throw std::runtime_error("patch state publication failed");};
    auto capture=[&](){ofJson state;
        state["audioReactiveControlSwitch"]=audioReactiveControlSwitch;
        state["videoReactiveSwitch"]=videoReactiveSwitch;
        state["inputGain"]=inputGain;
        state["blackoutAmount"]=blackoutAmount;
        state["wet_dry_switch"]=wet_dry_switch;
        state["toroidSwitch"]=toroidSwitch;
        state["mirrorSwitch"]=mirrorSwitch;
        state["lumakeyInvertSwitch"]=lumakeyInvertSwitch;
        state["moshBase"]=moshBase;
        state["chromaBase"]=chromaBase;
        state["crushBase"]=crushBase;
        state["pixelBase"]=pixelBase;
        state["holdBase"]=holdBase;
        state["holdDepth"]={holdDepth[0],holdDepth[1],holdDepth[2]};
        state["inputProcessingBase"]=ofJson::array();
        state["inputProcessingDepth"]=ofJson::array();
        for(int i=0;i<5;++i)state["inputProcessingBase"].push_back(inputProcessingBase[i]);
        for(int band=0;band<3;++band){
            ofJson values=ofJson::array();
            for(int i=0;i<5;++i)values.push_back(inputProcessingDepth[band][i]);
            state["inputProcessingDepth"].push_back(values);
        }
        state["clear_switch"]=clear_switch;
        state["lumaTrackEnabled"]=lumaTrackEnabled;
        state["lumaTrackReferencePending"]=lumaTrackReferencePending;
        state["lumaTrackReference"]=lumaTrackReference;
        state["showOverlayRequestedMode"]=showOverlayRequestedMode;
        state["forceMidiWrite"]=forceMidiWrite;
        state["forceMidiWriteTimer"]=forceMidiWriteTimer;
        state["c_lumakey_value"]=c_lumakey_value;
        state["c_mix"]=c_mix;
        state["c_hue"]=c_hue;
        state["c_sat"]=c_sat;
        state["c_bright"]=c_bright;
        state["c_x"]=c_x;
        state["c_y"]=c_y;
        state["c_z"]=c_z;
        state["c_rotate"]=c_rotate;
        state["c_huex_off"]=c_huex_off;
        state["c_huex_lfo"]=c_huex_lfo;
        state["brightInvert"]=brightInvert;
        state["highC1"]=highC1;
        state["highC10"]=highC10;
        state["highC11"]=highC11;
        state["highC12"]=highC12;
        state["highC13"]=highC13;
        state["highC14"]=highC14;
        state["highC15"]=highC15;
        state["highC16"]=highC16;
        state["highC2"]=highC2;
        state["highC3"]=highC3;
        state["highC4"]=highC4;
        state["highC5"]=highC5;
        state["highC6"]=highC6;
        state["highC7"]=highC7;
        state["highC8"]=highC8;
        state["highC9"]=highC9;
        state["horizontalMirror"]=horizontalMirror;
        state["hueInvert"]=hueInvert;
        state["huexx_0"]=huexx_0;
        state["huexx_1"]=huexx_1;
        state["huexx_2"]=huexx_2;
        state["huexy_0"]=huexy_0;
        state["huexy_1"]=huexy_1;
        state["huexy_2"]=huexy_2;
        state["huexz_0"]=huexz_0;
        state["huexz_1"]=huexz_1;
        state["huexz_2"]=huexz_2;
        state["lowC1"]=lowC1;
        state["lowC10"]=lowC10;
        state["lowC11"]=lowC11;
        state["lowC12"]=lowC12;
        state["lowC13"]=lowC13;
        state["lowC14"]=lowC14;
        state["lowC15"]=lowC15;
        state["lowC16"]=lowC16;
        state["lowC2"]=lowC2;
        state["lowC3"]=lowC3;
        state["lowC4"]=lowC4;
        state["lowC5"]=lowC5;
        state["lowC6"]=lowC6;
        state["lowC7"]=lowC7;
        state["lowC8"]=lowC8;
        state["lowC9"]=lowC9;
        state["midC1"]=midC1;
        state["midC10"]=midC10;
        state["midC11"]=midC11;
        state["midC12"]=midC12;
        state["midC13"]=midC13;
        state["midC14"]=midC14;
        state["midC15"]=midC15;
        state["midC16"]=midC16;
        state["midC2"]=midC2;
        state["midC3"]=midC3;
        state["midC4"]=midC4;
        state["midC5"]=midC5;
        state["midC6"]=midC6;
        state["midC7"]=midC7;
        state["midC8"]=midC8;
        state["midC9"]=midC9;
        state["r_10"]=r_10;
        state["r_2"]=r_2;
        state["r_5"]=r_5;
        state["saturationInvert"]=saturationInvert;
        state["vBright"]=vBright;
        state["vHue"]=vHue;
        state["vHuexLfo"]=vHuexLfo;
        state["vHuexMod"]=vHuexMod;
        state["vHuexOff"]=vHuexOff;
        state["vLumakeyValue"]=vLumakeyValue;
        state["vMix"]=vMix;
        state["vRotate"]=vRotate;
        state["vSaturation"]=vSaturation;
        state["vSharpenAmount"]=vSharpenAmount;
        state["vTemporalFilterMix"]=vTemporalFilterMix;
        state["vTemporalFilterResonance"]=vTemporalFilterResonance;
        state["vX"]=vX;
        state["vY"]=vY;
        state["vZ"]=vZ;
        state["verticalMirror"]=verticalMirror;
        state["x_10"]=x_10;
        state["x_2"]=x_2;
        state["x_5"]=x_5;
        state["y_10"]=y_10;
        state["y_2"]=y_2;
        state["y_5"]=y_5;
        state["z_10"]=z_10;
        state["z_2"]=z_2;
        state["z_5"]=z_5;
        state["lowC17"]=lowC17;
        state["lowC18"]=lowC18;
        state["lowC19"]=lowC19;
        state["lowC20"]=lowC20;
        state["midC17"]=midC17;
        state["midC18"]=midC18;
        state["midC19"]=midC19;
        state["midC20"]=midC20;
        state["highC17"]=highC17;
        state["highC18"]=highC18;
        state["highC19"]=highC19;
        state["highC20"]=highC20;
        state["manualControlTargets"]=ofJson::array();for(int i=0;i<17;++i)state["manualControlTargets"].push_back(manualControlTargets[i]);
        state["manualControlSmoothed"]=ofJson::array();for(int i=0;i<17;++i)state["manualControlSmoothed"].push_back(manualControlSmoothed[i]);
        state["midiActiveFloat"]=ofJson::array();for(int i=0;i<17;++i)state["midiActiveFloat"].push_back(midiActiveFloat[i]);
        state["vmidiActiveFloat"]=ofJson::array();for(int i=0;i<17;++i)state["vmidiActiveFloat"].push_back(vmidiActiveFloat[i]);
        state["midiLowActiveFloat"]=ofJson::array();for(int i=0;i<17;++i)state["midiLowActiveFloat"].push_back(midiLowActiveFloat[i]);
        state["midiMidActiveFloat"]=ofJson::array();for(int i=0;i<17;++i)state["midiMidActiveFloat"].push_back(midiMidActiveFloat[i]);
        state["midiHighActiveFloat"]=ofJson::array();for(int i=0;i<17;++i)state["midiHighActiveFloat"].push_back(midiHighActiveFloat[i]);
        return ofJson{{"kind","restore"},{"state",state}};
    };
    auto validateVideo=[&](const ofJson& value){
        autoeyez::VideoPatch p;
        if(!value.is_object()||value.size()!=autoeyez::patchFieldCount)throw std::runtime_error("video patch field count");
        for(auto it=value.begin();it!=value.end();++it){if(!it.value().is_number_integer()||!p.put(it.key().c_str(),it.value().get<int>()))throw std::runtime_error("video patch field/range");}
        if(!p.finish(p.get("slot")))throw std::runtime_error("video patch threshold pair");return p;
    };
    inbox.poll([&](const ofJson& payload)->std::pair<ofJson,ofJson>{
        const auto kind=payload.at("kind").get<std::string>();
        if(kind=="video") {validateVideo(payload.at("patch"));return {payload,capture()};}
        if(kind=="audio_save"||kind=="audio_load"){
            int slot=payload.at("slot").get<int>();fftEnvelopeRuntime.poll(ofGetElapsedTimeMillis(),true);std::string before=audioPatchText(slot),text=before;
            if(kind=="audio_load"){
                std::ifstream file("/home/pi/audiopatches/AUDIO"+ofToString(slot,0,2,'0')+".TXT");file.seekg(0,std::ios::end);const auto length=file.tellg();if(length<0||length>16384)throw std::runtime_error("audio patch size");file.seekg(0);std::ostringstream read;read<<file.rdbuf();if(!file)throw std::runtime_error("audio patch unavailable");text=read.str();
            }
            autoeyez::AudioPatch p;if(!autoeyez::readAudioPatch(text,slot,p))throw std::runtime_error("audio patch schema");
            return {ofJson{{"kind",kind},{"slot",slot},{"text",text}},ofJson{{"kind","audio_restore"},{"slot",slot},{"text",before},{"candidate_text",text}}};
        }
        throw std::runtime_error("unknown patch operation");
    },[&](const ofJson& payload){
        const auto kind=payload.at("kind").get<std::string>();
        if(kind=="restore"){
            auto state=payload.at("state");
            const auto restored_audioReactiveControlSwitch=state.at("audioReactiveControlSwitch").get<decltype(audioReactiveControlSwitch)>();
            const auto restored_videoReactiveSwitch=state.at("videoReactiveSwitch").get<decltype(videoReactiveSwitch)>();
            const auto restored_inputGain=state.at("inputGain").get<decltype(inputGain)>();
            const auto restored_blackoutAmount=state.at("blackoutAmount").get<decltype(blackoutAmount)>();
            const auto restored_wet_dry_switch=state.at("wet_dry_switch").get<decltype(wet_dry_switch)>();
            const auto restored_toroidSwitch=state.at("toroidSwitch").get<decltype(toroidSwitch)>();
            const auto restored_mirrorSwitch=state.at("mirrorSwitch").get<decltype(mirrorSwitch)>();
            const auto restored_lumakeyInvertSwitch=state.at("lumakeyInvertSwitch").get<decltype(lumakeyInvertSwitch)>();
            const auto restored_moshBase=state.at("moshBase").get<decltype(moshBase)>();
            const auto restored_chromaBase=state.at("chromaBase").get<decltype(chromaBase)>();
            const auto restored_crushBase=state.at("crushBase").get<decltype(crushBase)>();
            const auto restored_pixelBase=state.at("pixelBase").get<decltype(pixelBase)>();
            const auto restored_holdBase=state.at("holdBase").get<decltype(holdBase)>();
            const auto restored_clear_switch=state.at("clear_switch").get<decltype(clear_switch)>();
            const auto restored_lumaTrackEnabled=state.at("lumaTrackEnabled").get<decltype(lumaTrackEnabled)>();
            const auto restored_lumaTrackReferencePending=state.at("lumaTrackReferencePending").get<decltype(lumaTrackReferencePending)>();
            const auto restored_lumaTrackReference=state.at("lumaTrackReference").get<decltype(lumaTrackReference)>();
            const auto restored_showOverlayRequestedMode=state.at("showOverlayRequestedMode").get<decltype(showOverlayRequestedMode)>();
            const auto restored_forceMidiWrite=state.at("forceMidiWrite").get<decltype(forceMidiWrite)>();
            const auto restored_forceMidiWriteTimer=state.at("forceMidiWriteTimer").get<decltype(forceMidiWriteTimer)>();
            const auto restored_c_lumakey_value=state.at("c_lumakey_value").get<decltype(c_lumakey_value)>();
            const auto restored_c_mix=state.at("c_mix").get<decltype(c_mix)>();
            const auto restored_c_hue=state.at("c_hue").get<decltype(c_hue)>();
            const auto restored_c_sat=state.at("c_sat").get<decltype(c_sat)>();
            const auto restored_c_bright=state.at("c_bright").get<decltype(c_bright)>();
            const auto restored_c_x=state.at("c_x").get<decltype(c_x)>();
            const auto restored_c_y=state.at("c_y").get<decltype(c_y)>();
            const auto restored_c_z=state.at("c_z").get<decltype(c_z)>();
            const auto restored_c_rotate=state.at("c_rotate").get<decltype(c_rotate)>();
            const auto restored_c_huex_off=state.at("c_huex_off").get<decltype(c_huex_off)>();
            const auto restored_c_huex_lfo=state.at("c_huex_lfo").get<decltype(c_huex_lfo)>();
            const auto restored_brightInvert=state.at("brightInvert").get<decltype(brightInvert)>();
            const auto restored_highC1=state.at("highC1").get<decltype(highC1)>();
            const auto restored_highC10=state.at("highC10").get<decltype(highC10)>();
            const auto restored_highC11=state.at("highC11").get<decltype(highC11)>();
            const auto restored_highC12=state.at("highC12").get<decltype(highC12)>();
            const auto restored_highC13=state.at("highC13").get<decltype(highC13)>();
            const auto restored_highC14=state.at("highC14").get<decltype(highC14)>();
            const auto restored_highC15=state.at("highC15").get<decltype(highC15)>();
            const auto restored_highC16=state.at("highC16").get<decltype(highC16)>();
            const auto restored_highC2=state.at("highC2").get<decltype(highC2)>();
            const auto restored_highC3=state.at("highC3").get<decltype(highC3)>();
            const auto restored_highC4=state.at("highC4").get<decltype(highC4)>();
            const auto restored_highC5=state.at("highC5").get<decltype(highC5)>();
            const auto restored_highC6=state.at("highC6").get<decltype(highC6)>();
            const auto restored_highC7=state.at("highC7").get<decltype(highC7)>();
            const auto restored_highC8=state.at("highC8").get<decltype(highC8)>();
            const auto restored_highC9=state.at("highC9").get<decltype(highC9)>();
            const auto restored_horizontalMirror=state.at("horizontalMirror").get<decltype(horizontalMirror)>();
            const auto restored_hueInvert=state.at("hueInvert").get<decltype(hueInvert)>();
            const auto restored_huexx_0=state.at("huexx_0").get<decltype(huexx_0)>();
            const auto restored_huexx_1=state.at("huexx_1").get<decltype(huexx_1)>();
            const auto restored_huexx_2=state.at("huexx_2").get<decltype(huexx_2)>();
            const auto restored_huexy_0=state.at("huexy_0").get<decltype(huexy_0)>();
            const auto restored_huexy_1=state.at("huexy_1").get<decltype(huexy_1)>();
            const auto restored_huexy_2=state.at("huexy_2").get<decltype(huexy_2)>();
            const auto restored_huexz_0=state.at("huexz_0").get<decltype(huexz_0)>();
            const auto restored_huexz_1=state.at("huexz_1").get<decltype(huexz_1)>();
            const auto restored_huexz_2=state.at("huexz_2").get<decltype(huexz_2)>();
            const auto restored_lowC1=state.at("lowC1").get<decltype(lowC1)>();
            const auto restored_lowC10=state.at("lowC10").get<decltype(lowC10)>();
            const auto restored_lowC11=state.at("lowC11").get<decltype(lowC11)>();
            const auto restored_lowC12=state.at("lowC12").get<decltype(lowC12)>();
            const auto restored_lowC13=state.at("lowC13").get<decltype(lowC13)>();
            const auto restored_lowC14=state.at("lowC14").get<decltype(lowC14)>();
            const auto restored_lowC15=state.at("lowC15").get<decltype(lowC15)>();
            const auto restored_lowC16=state.at("lowC16").get<decltype(lowC16)>();
            const auto restored_lowC2=state.at("lowC2").get<decltype(lowC2)>();
            const auto restored_lowC3=state.at("lowC3").get<decltype(lowC3)>();
            const auto restored_lowC4=state.at("lowC4").get<decltype(lowC4)>();
            const auto restored_lowC5=state.at("lowC5").get<decltype(lowC5)>();
            const auto restored_lowC6=state.at("lowC6").get<decltype(lowC6)>();
            const auto restored_lowC7=state.at("lowC7").get<decltype(lowC7)>();
            const auto restored_lowC8=state.at("lowC8").get<decltype(lowC8)>();
            const auto restored_lowC9=state.at("lowC9").get<decltype(lowC9)>();
            const auto restored_midC1=state.at("midC1").get<decltype(midC1)>();
            const auto restored_midC10=state.at("midC10").get<decltype(midC10)>();
            const auto restored_midC11=state.at("midC11").get<decltype(midC11)>();
            const auto restored_midC12=state.at("midC12").get<decltype(midC12)>();
            const auto restored_midC13=state.at("midC13").get<decltype(midC13)>();
            const auto restored_midC14=state.at("midC14").get<decltype(midC14)>();
            const auto restored_midC15=state.at("midC15").get<decltype(midC15)>();
            const auto restored_midC16=state.at("midC16").get<decltype(midC16)>();
            const auto restored_midC2=state.at("midC2").get<decltype(midC2)>();
            const auto restored_midC3=state.at("midC3").get<decltype(midC3)>();
            const auto restored_midC4=state.at("midC4").get<decltype(midC4)>();
            const auto restored_midC5=state.at("midC5").get<decltype(midC5)>();
            const auto restored_midC6=state.at("midC6").get<decltype(midC6)>();
            const auto restored_midC7=state.at("midC7").get<decltype(midC7)>();
            const auto restored_midC8=state.at("midC8").get<decltype(midC8)>();
            const auto restored_midC9=state.at("midC9").get<decltype(midC9)>();
            const auto restored_r_10=state.at("r_10").get<decltype(r_10)>();
            const auto restored_r_2=state.at("r_2").get<decltype(r_2)>();
            const auto restored_r_5=state.at("r_5").get<decltype(r_5)>();
            const auto restored_saturationInvert=state.at("saturationInvert").get<decltype(saturationInvert)>();
            const auto restored_vBright=state.at("vBright").get<decltype(vBright)>();
            const auto restored_vHue=state.at("vHue").get<decltype(vHue)>();
            const auto restored_vHuexLfo=state.at("vHuexLfo").get<decltype(vHuexLfo)>();
            const auto restored_vHuexMod=state.at("vHuexMod").get<decltype(vHuexMod)>();
            const auto restored_vHuexOff=state.at("vHuexOff").get<decltype(vHuexOff)>();
            const auto restored_vLumakeyValue=state.at("vLumakeyValue").get<decltype(vLumakeyValue)>();
            const auto restored_vMix=state.at("vMix").get<decltype(vMix)>();
            const auto restored_vRotate=state.at("vRotate").get<decltype(vRotate)>();
            const auto restored_vSaturation=state.at("vSaturation").get<decltype(vSaturation)>();
            const auto restored_vSharpenAmount=state.at("vSharpenAmount").get<decltype(vSharpenAmount)>();
            const auto restored_vTemporalFilterMix=state.at("vTemporalFilterMix").get<decltype(vTemporalFilterMix)>();
            const auto restored_vTemporalFilterResonance=state.at("vTemporalFilterResonance").get<decltype(vTemporalFilterResonance)>();
            const auto restored_vX=state.at("vX").get<decltype(vX)>();
            const auto restored_vY=state.at("vY").get<decltype(vY)>();
            const auto restored_vZ=state.at("vZ").get<decltype(vZ)>();
            const auto restored_verticalMirror=state.at("verticalMirror").get<decltype(verticalMirror)>();
            const auto restored_x_10=state.at("x_10").get<decltype(x_10)>();
            const auto restored_x_2=state.at("x_2").get<decltype(x_2)>();
            const auto restored_x_5=state.at("x_5").get<decltype(x_5)>();
            const auto restored_y_10=state.at("y_10").get<decltype(y_10)>();
            const auto restored_y_2=state.at("y_2").get<decltype(y_2)>();
            const auto restored_y_5=state.at("y_5").get<decltype(y_5)>();
            const auto restored_z_10=state.at("z_10").get<decltype(z_10)>();
            const auto restored_z_2=state.at("z_2").get<decltype(z_2)>();
            const auto restored_z_5=state.at("z_5").get<decltype(z_5)>();
            const auto restored_lowC17=state.at("lowC17").get<decltype(lowC17)>();
            const auto restored_lowC18=state.at("lowC18").get<decltype(lowC18)>();
            const auto restored_lowC19=state.at("lowC19").get<decltype(lowC19)>();
            const auto restored_lowC20=state.at("lowC20").get<decltype(lowC20)>();
            const auto restored_midC17=state.at("midC17").get<decltype(midC17)>();
            const auto restored_midC18=state.at("midC18").get<decltype(midC18)>();
            const auto restored_midC19=state.at("midC19").get<decltype(midC19)>();
            const auto restored_midC20=state.at("midC20").get<decltype(midC20)>();
            const auto restored_highC17=state.at("highC17").get<decltype(highC17)>();
            const auto restored_highC18=state.at("highC18").get<decltype(highC18)>();
            const auto restored_highC19=state.at("highC19").get<decltype(highC19)>();
            const auto restored_highC20=state.at("highC20").get<decltype(highC20)>();
            typename std::remove_reference<decltype(manualControlTargets[0])>::type restored_manualControlTargets[17];for(int i=0;i<17;++i)restored_manualControlTargets[i]=state.at("manualControlTargets").at(i).get<typename std::remove_reference<decltype(manualControlTargets[i])>::type>();
            typename std::remove_reference<decltype(manualControlSmoothed[0])>::type restored_manualControlSmoothed[17];for(int i=0;i<17;++i)restored_manualControlSmoothed[i]=state.at("manualControlSmoothed").at(i).get<typename std::remove_reference<decltype(manualControlSmoothed[i])>::type>();
            typename std::remove_reference<decltype(midiActiveFloat[0])>::type restored_midiActiveFloat[17];for(int i=0;i<17;++i)restored_midiActiveFloat[i]=state.at("midiActiveFloat").at(i).get<typename std::remove_reference<decltype(midiActiveFloat[i])>::type>();
            typename std::remove_reference<decltype(vmidiActiveFloat[0])>::type restored_vmidiActiveFloat[17];for(int i=0;i<17;++i)restored_vmidiActiveFloat[i]=state.at("vmidiActiveFloat").at(i).get<typename std::remove_reference<decltype(vmidiActiveFloat[i])>::type>();
            typename std::remove_reference<decltype(midiLowActiveFloat[0])>::type restored_midiLowActiveFloat[17];for(int i=0;i<17;++i)restored_midiLowActiveFloat[i]=state.at("midiLowActiveFloat").at(i).get<typename std::remove_reference<decltype(midiLowActiveFloat[i])>::type>();
            typename std::remove_reference<decltype(midiMidActiveFloat[0])>::type restored_midiMidActiveFloat[17];for(int i=0;i<17;++i)restored_midiMidActiveFloat[i]=state.at("midiMidActiveFloat").at(i).get<typename std::remove_reference<decltype(midiMidActiveFloat[i])>::type>();
            typename std::remove_reference<decltype(midiHighActiveFloat[0])>::type restored_midiHighActiveFloat[17];for(int i=0;i<17;++i)restored_midiHighActiveFloat[i]=state.at("midiHighActiveFloat").at(i).get<typename std::remove_reference<decltype(midiHighActiveFloat[i])>::type>();
            audioReactiveControlSwitch=restored_audioReactiveControlSwitch;
            videoReactiveSwitch=restored_videoReactiveSwitch;
            inputGain=restored_inputGain;
            blackoutAmount=restored_blackoutAmount;
            wet_dry_switch=restored_wet_dry_switch;
            toroidSwitch=restored_toroidSwitch;
            mirrorSwitch=restored_mirrorSwitch;
            lumakeyInvertSwitch=restored_lumakeyInvertSwitch;
            moshBase=restored_moshBase;
            chromaBase=restored_chromaBase;
            crushBase=restored_crushBase;
            pixelBase=restored_pixelBase;
            holdBase=restored_holdBase;
            for(int band=0;band<3;++band)holdDepth[band]=state.at("holdDepth").at(band).get<float>();
            for(int i=0;i<5;++i)inputProcessingBase[i]=state.at("inputProcessingBase").at(i).get<float>();
            for(int band=0;band<3;++band)
                for(int i=0;i<5;++i)
                    inputProcessingDepth[band][i]=state.at("inputProcessingDepth").at(band).at(i).get<float>();
            clear_switch=restored_clear_switch;
            lumaTrackEnabled=restored_lumaTrackEnabled;
            lumaTrackReferencePending=restored_lumaTrackReferencePending;
            lumaTrackReference=restored_lumaTrackReference;
            showOverlayRequestedMode=restored_showOverlayRequestedMode;
            forceMidiWrite=restored_forceMidiWrite;
            forceMidiWriteTimer=restored_forceMidiWriteTimer;
            c_lumakey_value=restored_c_lumakey_value;
            c_mix=restored_c_mix;
            c_hue=restored_c_hue;
            c_sat=restored_c_sat;
            c_bright=restored_c_bright;
            c_x=restored_c_x;
            c_y=restored_c_y;
            c_z=restored_c_z;
            c_rotate=restored_c_rotate;
            c_huex_off=restored_c_huex_off;
            c_huex_lfo=restored_c_huex_lfo;
            brightInvert=restored_brightInvert;
            highC1=restored_highC1;
            highC10=restored_highC10;
            highC11=restored_highC11;
            highC12=restored_highC12;
            highC13=restored_highC13;
            highC14=restored_highC14;
            highC15=restored_highC15;
            highC16=restored_highC16;
            highC2=restored_highC2;
            highC3=restored_highC3;
            highC4=restored_highC4;
            highC5=restored_highC5;
            highC6=restored_highC6;
            highC7=restored_highC7;
            highC8=restored_highC8;
            highC9=restored_highC9;
            horizontalMirror=restored_horizontalMirror;
            hueInvert=restored_hueInvert;
            huexx_0=restored_huexx_0;
            huexx_1=restored_huexx_1;
            huexx_2=restored_huexx_2;
            huexy_0=restored_huexy_0;
            huexy_1=restored_huexy_1;
            huexy_2=restored_huexy_2;
            huexz_0=restored_huexz_0;
            huexz_1=restored_huexz_1;
            huexz_2=restored_huexz_2;
            lowC1=restored_lowC1;
            lowC10=restored_lowC10;
            lowC11=restored_lowC11;
            lowC12=restored_lowC12;
            lowC13=restored_lowC13;
            lowC14=restored_lowC14;
            lowC15=restored_lowC15;
            lowC16=restored_lowC16;
            lowC2=restored_lowC2;
            lowC3=restored_lowC3;
            lowC4=restored_lowC4;
            lowC5=restored_lowC5;
            lowC6=restored_lowC6;
            lowC7=restored_lowC7;
            lowC8=restored_lowC8;
            lowC9=restored_lowC9;
            midC1=restored_midC1;
            midC10=restored_midC10;
            midC11=restored_midC11;
            midC12=restored_midC12;
            midC13=restored_midC13;
            midC14=restored_midC14;
            midC15=restored_midC15;
            midC16=restored_midC16;
            midC2=restored_midC2;
            midC3=restored_midC3;
            midC4=restored_midC4;
            midC5=restored_midC5;
            midC6=restored_midC6;
            midC7=restored_midC7;
            midC8=restored_midC8;
            midC9=restored_midC9;
            r_10=restored_r_10;
            r_2=restored_r_2;
            r_5=restored_r_5;
            saturationInvert=restored_saturationInvert;
            vBright=restored_vBright;
            vHue=restored_vHue;
            vHuexLfo=restored_vHuexLfo;
            vHuexMod=restored_vHuexMod;
            vHuexOff=restored_vHuexOff;
            vLumakeyValue=restored_vLumakeyValue;
            vMix=restored_vMix;
            vRotate=restored_vRotate;
            vSaturation=restored_vSaturation;
            vSharpenAmount=restored_vSharpenAmount;
            vTemporalFilterMix=restored_vTemporalFilterMix;
            vTemporalFilterResonance=restored_vTemporalFilterResonance;
            vX=restored_vX;
            vY=restored_vY;
            vZ=restored_vZ;
            verticalMirror=restored_verticalMirror;
            x_10=restored_x_10;
            x_2=restored_x_2;
            x_5=restored_x_5;
            y_10=restored_y_10;
            y_2=restored_y_2;
            y_5=restored_y_5;
            z_10=restored_z_10;
            z_2=restored_z_2;
            z_5=restored_z_5;
            lowC17=restored_lowC17;
            lowC18=restored_lowC18;
            lowC19=restored_lowC19;
            lowC20=restored_lowC20;
            midC17=restored_midC17;
            midC18=restored_midC18;
            midC19=restored_midC19;
            midC20=restored_midC20;
            highC17=restored_highC17;
            highC18=restored_highC18;
            highC19=restored_highC19;
            highC20=restored_highC20;
            for(int i=0;i<17;++i)manualControlTargets[i]=restored_manualControlTargets[i];
            for(int i=0;i<17;++i)manualControlSmoothed[i]=restored_manualControlSmoothed[i];
            for(int i=0;i<17;++i)midiActiveFloat[i]=restored_midiActiveFloat[i];
            for(int i=0;i<17;++i)vmidiActiveFloat[i]=restored_vmidiActiveFloat[i];
            for(int i=0;i<17;++i)midiLowActiveFloat[i]=restored_midiLowActiveFloat[i];
            for(int i=0;i<17;++i)midiMidActiveFloat[i]=restored_midiMidActiveFloat[i];
            for(int i=0;i<17;++i)midiHighActiveFloat[i]=restored_midiHighActiveFloat[i];
            writeState("/dev/shm/luma_track",lumaTrackEnabled?1:0);writeState("/dev/shm/show_overlay",showOverlayRequestedMode);
            return;
        }
        if(kind=="audio_load"||kind=="audio_restore"){applyAudioPatchText(payload.at("text").get<std::string>(),payload.at("slot").get<int>());return;}
        if(kind=="audio_save")return;
        if(kind!="video")throw std::runtime_error("patch commit operation");
        const auto p=validateVideo(payload.at("patch"));std::vector<ofxMidiMessage> batch;
        auto cc=[&](int control,int value){ofxMidiMessage m;m.status=MIDI_CONTROL_CHANGE;m.channel=1;m.control=control;m.value=value;batch.push_back(m);};
        // Keep the existing recall message order and routing semantics.
        // The entire sequence completes before the next frame is rendered.
        cc(91,127);
        const int encoders[]={16,17,18,19,120,121,122,123,7,20,21,22,23,124,125,126,127};
        for(int i=0;i<17;++i){int value=p.get(("enc"+std::to_string(i)).c_str());cc(encoders[i],i==8?value*32/127:value);}
        const int controls[]={60,46,62,61,41,45,71};
        for(int i=0;i<7;++i)cc(controls[i],p.get(("ctrl"+std::to_string(i)).c_str())?127:0);
        for(int j=0;j<8;++j){for(int m=1;m<=3;++m)cc(16+16*m+j,0);int selected=p.get(("func"+std::to_string(j)).c_str());if(selected)cc(16+16*selected+j,127);}
        cc(100,p.get("renderLumaInvert")?127:0);cc(101,p.get("renderMirror")?127:0);
        const int reactive[]={43,44,42};for(int control:reactive)cc(control,0);
        cc(94,p.get("mosh"));cc(95,p.get("chroma"));cc(96,p.get("crush"));cc(97,p.get("pixel"));
        cc(102,p.get("bleed"));cc(103,p.get("align"));cc(104,p.get("ghost"));
        cc(105,p.get("wobble"));cc(106,p.get("overdrive"));
        cc(107,p.get("hold"));
        if(p.get("react")>=0)cc(reactive[p.get("react")],127);
        cc(91,0);midibiz(&batch);
        const bool tracking=p.get("lumaTrack")!=0;
        if(tracking!=lumaTrackEnabled){lumaTrackEnabled=tracking;lumaTrackReferencePending=tracking;}
        showOverlayRequestedMode=p.get("mainLogo")?3:p.get("houseLogo")?2:p.get("houseCard")?1:0;
        writeState("/dev/shm/luma_track",tracking?1:0);writeState("/dev/shm/show_overlay",showOverlayRequestedMode);
        ofLogNotice()<<"PATCH_TRANSACTION video committed slot="<<p.get("slot");
    });
}
