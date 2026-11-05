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
	void midiSetup();
	ofxMidiIn midiIn;
	std::vector<ofxMidiMessage> midiMessages;
	std::size_t maxMessages = 10; //< max number of messages to keep track of

	ofShader shader_mixer;
	ofShader shaderSharpen;

	void fbDeclareAndAllocate();
    ofFbo framebuffer0;
    ofFbo framebuffer1;
    ofFbo sharpenFramebuffer;
    
    void inputSetup();
    void inputUpdate();
    ofVideoGrabber cam1;

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
