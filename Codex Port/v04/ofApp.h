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

#include "ofMain.h"
#include "ofxMidi.h"
//#include "ofxOMXVideoGrabber.h"
#include "ofxProcessFFT.h"
#include <mutex>

class ofApp : public ofBaseApp, public ofxMidiListener {
	
public:
	
	void setup();
	void update();
	void draw();
	void exit();
	
        void fillPLockFromCurrent() ;
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
	void midibiz();
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
		void midiSetup();
		bool openPreferredMidiPort();
	ofxMidiIn midiIn;
	std::vector<ofxMidiMessage> midiMessages;
	std::mutex midiMessagesMutex;
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

	void fbDeclareAndAllocate();
    ofFbo framebuffer0;
    ofFbo framebuffer1;
    ofFbo sharpenFramebuffer;
    ofFbo postFxFramebuffer[2];
    ofFbo moshFrame[2];       // Consecutive 480x270 frames for block matching
    ofFbo moshMotion;         // 120x68 encoded motion-vector field
    ofFbo moshAccum[2];      // Full-resolution recursive image
    int moshFrameIndex = 0;
    int moshAccumIndex = 0;
    unsigned int moshFrameCount = 0;
    bool moshPrimed = false;
    
    void inputSetup();
    void inputUpdate();
    void pollLumaTrackState();
    void updateLumaTrackMeasurement();
    ofVideoGrabber cam1;

    unsigned int lumaTrackInputFrames = 0;
    bool lumaTrackEnabled = false;
    bool lumaTrackReferencePending = false;
    bool lumaTrackMeasurementReady = false;
    float lumaTrackSmoothed = 0.0f;
    float lumaTrackReference = 0.0f;
    unsigned long lumaTrackStatePollMs = 0;
    unsigned long lumaTrackLogMs = 0;

    // Loading state (shows "LOADING..." until video ready)
    bool videoInitialized = false;
    bool videoReady = false;
    unsigned long lastFrameTime = 0;
    int loadingDotState = 0;
    unsigned long lastDotUpdate = 0;
    bool checkSocketExists();
    void drawLoading();
    
    void p_lockClear();
    void p_lockUpdate();

    ProcessFFT fft;
    void fftAssignValues();
    float inputGain = 1.0f;
    
    void parametersAssign();
    void midiLatchClear();
};
