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
#pragma once
#include "input_recovery.h"
#include "real_frame_grabber.h"
#ifdef AUTOEYEZ_D3_TRACE_CANDIDATE
#include "trace_frame_grabber.h"
#endif

#include "ofMain.h"
#include "midi_burst_queue.h"
#include "midi_fft_frame.h"
#include <atomic>
#include <chrono>
#include "ofxMidi.h"
//#include "ofxOMXVideoGrabber.h"
#include "ofxProcessFFT.h"
#include <mutex>

class ofApp : public ofBaseApp, public ofxMidiListener {
	
public:
#ifdef AUTOEYEZ_D3_TRACE_CANDIDATE
    std::shared_ptr<TraceFrameGrabber> traceInput;
#endif
	
	void setup();
	void update();
	void draw();
	void exit();
	// Shared GL texture for the separate HDMI-2 internal preview window.
	ofTexture* internalPreviewTexture() {
		return previewSource && previewSource->isAllocated()
		    ? &previewSource->getTexture() : nullptr;
	}
	float internalPreviewFps = 30.0f;
	ofFbo* previewSource = nullptr;
	
        void saveAudioPatch(int slot) ;
        void loadAudioPatch(int slot) ;

	void keyPressed(int key);
	void keyReleased(int key);
	/*
	void omx_settings();
	void omx_updates();
	ofxOMXCameraSettings settings;
    ofxOMXVideoGrabber videoGrabber;
	*/
	void midibiz(const std::vector<ofxMidiMessage>* patchMessages=nullptr);
	void midibizOld();
	void newMidiMessage(ofxMidiMessage& eventArgs);
	void forceCcSetup();
	void forceCcPoll();
	int forceCcSocket = -1;
	float forceCcOffset[128] = {};
	void pollShowOverlayState();
	int showOverlayMode = 0;  // 0=off, 1=house card, 2=house logo, 3=main logo
	int showOverlayRequestedMode = 0;
	float showOverlayAlpha = 0.0f;
	unsigned long showOverlayFadeMs = 0;
	unsigned long showOverlayPollMs = 0;
	ofImage houseCardImage;
	ofImage houseLogoImage;
	ofImage mainLogoImage;
	// Independent Low, Mid, High image decks sampled by the mixer luma key.
	std::vector<ofImage> peekImages[3];
	float peekBase = 0.0f;
	int peekLastPhysicalBaseCc = -1;
	bool peekBaseSetByNote = false;
	float peekDepth[3] = {1.0f, 1.0f, 1.0f};
	float peekHit[3] = {};
	float peekPulseHeldLevel = 0.0f;
	float peekPulseOffset = 0.0f;
	bool peekPulseHeld = false;
	unsigned long peekPulseLastSeenMs = 0;
	int peekMaskMode = 0; // 0: image alpha, 1: image luminance
	unsigned int peekImageIndex[3] = {};
	unsigned int peekNextFlip[3] = {};
	int peekStutterFrames[2] = {};
	unsigned int peekStutterStartFrame = 0;
	unsigned int peekStutterStartIndex[3] = {};
	void loadPeekImages();
		void midiSetup();
		bool openPreferredMidiPort();
	ofxMidiIn midiIn;
	std::vector<ofxMidiMessage> midiMessages;
	std::mutex midiMessagesMutex;
    std::mutex midiFftMutex;
    autoeyez::MidiFftFrame midiFftFrame;
    std::chrono::steady_clock::time_point midiFftAt{};
    std::atomic<unsigned long> midiLastMessageMs{0};
    autoeyez::MidiBurstQueue<ofxMidiMessage> midiBurstQueue;
    std::atomic<bool> midiPreserveRepeatedControls{false};
    unsigned long midiQueueReportMs=0;
    void expirePatchWriteLatch(unsigned long now);
    void patchTransactionPoll();
    std::string audioPatchText(int slot);
    void applyAudioPatchText(const std::string& text,int slot);
	std::size_t maxMessages = 10; //< max number of messages to keep track of

	ofShader shader_mixer;
	ofShader shaderPassthrough;  // Pi 5 passthrough for FBO copies
    ofVboMesh clipSpaceQuad;  // Pi 5 clip-space rendering
	ofShader shaderSharpen;
	ofShader shaderPixel;
	ofShader shaderChroma;
	ofShader shaderCrush;
	ofShader shaderMoshMotion;
	ofShader shaderMosh;
	ofShader shaderInputProcessing;
	ofShader shaderFrameHold;

	void fbDeclareAndAllocate();
    ofFbo framebuffer0;
    ofFbo framebuffer1;
    ofFbo sharpenFramebuffer;
    ofFbo postFxFramebuffer[2];
    ofFbo inputProcessingFramebuffer[2];
    int inputProcessingIndex = 0;
    bool inputProcessingHistoryValid = false;
    ofFbo moshFrame[2];       // Consecutive 480x270 frames for block matching
    ofFbo moshMotion;         // 120x68 encoded motion-vector field
    ofFbo moshAccum[2];      // Full-resolution recursive image
    int moshFrameIndex = 0;
    int moshAccumIndex = 0;
    unsigned int moshFrameCount = 0;
    bool moshPrimed = false;
    
    bool inputSetup();
    void updateVideoInput();
    bool loadVideoShaders();
    void publishAppStatus();
    std::string midiSerialGeneration;
    std::uint64_t lastStatusPublishMs = 0;
    std::uint64_t appInputFrames = 0;
    void inputUpdate();
    void pollLumaTrackState();
    void updateLumaTrackMeasurement();
    ofVideoGrabber cam1;
    bool realFrameGrabberInstalled = false;

    unsigned int lumaTrackInputFrames = 0;
    bool lumaTrackEnabled = false;
    bool lumaTrackReferencePending = false;
    bool lumaTrackMeasurementReady = false;
    float lumaTrackSmoothed = 0.0f;
    float lumaTrackReference = 0.0f;
    unsigned long lumaTrackStatePollMs = 0;
    unsigned long lumaTrackLogMs = 0;

    // Loading state (shows "LOADING..." until video ready)
    InputRecovery inputRecovery;
    bool videoShadersReady = false;
    std::uint64_t nextShaderRetryMs = 0;
    bool videoReady = false;
    int loadingDotState = 0;
    unsigned long lastDotUpdate = 0;
    bool checkSocketExists();
    void drawLoading();
    
    void clearManualControls();
    void updateManualControls();

    ProcessFFT fft;
    void fftAssignValues();
    float inputGain = 1.0f;
    
    void parametersAssign();
    void midiLatchClear();
};
