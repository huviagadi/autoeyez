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
 * autowaaave — GPU Video Feedback Processor
 * Part of the autoeyez video synthesis system
 *
 * Based on auto_waaave v1.5 by Andrei Jay (ex-zee-ex)
 * https://github.com/ex-zee-ex/auto_waaave
 *
 * Modified for integration with automidi controller and autoclip video mixer.
 * Receives MIDI CC from Teensy, video stream from autoclip, outputs to HDMI.
 */

#include "ofApp.h"

#include "iostream"

#define MIDI_MAGIC 63.50f

#define CONTROL_THRESHOLD .035f

//flip this switch to try different scalings
//0 is 320 1 is 640
//if you reduce scale to 320 you can up the total delay time to
//about 4 seconds/ 120 frames
//so try that out sometime and see how that feels!
bool forceMidiWrite = false ;
bool scaleswitch=1;
const int framebufferLength=60;
unsigned long forceMidiWriteTimer = 0 ;
unsigned long midiReconnectTimer = 0 ;
float blackoutAmount = 0 ;
float blackoutSmoothed = 0 ;
//const int framebufferLength=120;

//0 is picaputre, 1 is usbinput
bool inputswitch=1;

//0 is wet (framebuffer fed from final output, internal
//feedback mode
//1 is dry (framebuffer fed direct from camera input,
//traditional video delay mode
bool wet_dry_switch=1;

//0 is sd aspect ratio
//use definitely with all of the VSERPI devices 
//and anything else doing like 480i/p over hdmi
//1 is corner cropping to fill the screen
int hdmi_aspect_ratio_switch=0;

float az = 1.0;
float sx = 0;
float dc = 0;
float fv = 1;
float gb = 1;
float hn = 1;
float jm = 0.0;
float kk = 0.0;
float ll = 0.0;
float qw = 0.0;
float er = 1.0;
float ty = 0.0;
float ui = 0.0;
float op = 0.0;

float fb1_brightx=0.0;
bool toroidSwitch=0;
bool y_skew_switch=FALSE;
bool x_skew_switch=FALSE;
bool lumakeyInvertSwitch=FALSE;
bool x_mirrorSwitch=FALSE;
bool y_mirrorSwitch=FALSE;
float y_skew=0;
float x_skew=0;
bool mirrorSwitch=FALSE;

int fb0_delayamount=0;

//dummy variables for midi control

int width=640;
int height=480;


//dummy variables for midi to audio attenuatiors
//0 is direct midi, 1 is low_x, 2 is mid_x, 3 is high_x
int audioReactiveControlSwitch=0;

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


bool clear_switch=0;
//toggle switch variables
bool hueInvert=FALSE;
bool saturationInvert=FALSE;
bool brightInvert=FALSE;

bool cam1_h_invert=FALSE;
bool cam1_s_invert=FALSE;
bool cam1_b_invert=FALSE;

bool horizontalMirror=FALSE;
bool verticalMirror=FALSE;

bool clear_flip=FALSE;

bool x_2=FALSE;
bool x_5=FALSE;
bool x_10=FALSE;

bool y_2=FALSE;
bool y_5=FALSE;
bool y_10=FALSE;

bool z_2=FALSE;
bool z_5=FALSE;
bool z_10=FALSE;

bool r_2=FALSE;
bool r_5=FALSE;
bool r_10=FALSE;

bool huexx_0=FALSE;
bool huexx_1=FALSE;
bool huexx_2=FALSE;

bool huexy_0=FALSE;
bool huexy_1=FALSE;
bool huexy_2=FALSE;

bool huexz_0=FALSE;
bool huexz_1=FALSE;
bool huexz_2=FALSE;

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

//p_lock biz
//maximum total size of the plock array
const int p_lock_size=240;

bool p_lock_switch=0;

bool p_lock_erase=0;

//maximum number of p_locks available...maybe there can be one for every knob
//for whatever wacky reason the last member of this array of arrays has a glitch
//so i guess just make an extra array and forget about it for now
const int p_lock_number=17;

//so how we will organize the p_locks is in multidimensional arrays
//to access the data at timestep x for p_lock 2 (remember counting from 0) we use p_lock[2][x]
float p_lock[p_lock_number][p_lock_size];

//smoothing parameters(i think for all of these we can array both the arrays and the floats
//for now let us just try 1 smoothing parameter for everything.
float p_lock_smooth=.25;

//and then lets try an array of floats for storing the smoothed values
float p_lock_smoothed[p_lock_number];

//turn on and off writing to the array
bool p_lock_0_switch=1;

//global counter for all the locks
int p_lock_increment=0;

float my_normalize=0;

float low_value_smoothed=0.0;
float mid_value_smoothed=0.0;
float high_value_smoothed=0.0;
float smoothing_rate=.5;

ofFbo aspect_fix_fbo;
ofFbo dry_framebuffer;

bool videoReactiveSwitch=FALSE;

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
//--------------------------------------------------------------
void ofApp::setup() {
    //ofSetVerticalSync(true);
    ofSetFrameRate(30);
    ofBackground(0);
    //ofToggleFullscreen();
    ofHideCursor();
    //omx_settings();
    inputSetup();
    midiSetup();
    fbDeclareAndAllocate();
    shader_mixer.load("shadersES2/shader_mixer");
    shaderSharpen.load("shadersES2/shaderSharpen");
    //fft biz
    fft.setup();
    fft.setNormalize(false);
    //fft.setVolumeRange(1.0f);
    
    p_lockClear();
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

    if(ofGetElapsedTimeMillis() - midiReconnectTimer > 5000) {
        midiReconnectTimer = ofGetElapsedTimeMillis();
        if(!midiIn.isOpen() && midiIn.getNumInPorts() > 1) {
            midiIn.closePort();
            midiIn.removeListener(this);
            midiIn.openPort(1);
            midiIn.ignoreTypes(false, false, false);
            midiIn.addListener(this);
        }
    }

    fft.update();
    inputUpdate();
    midibiz();
    p_lockUpdate();
    
    if(fft.getLowVal()>my_normalize){
        my_normalize=fft.getLowVal();
    }
}

//--------------------------------------------------------------
void ofApp::draw() {
    
    
    //the range changes depending on what yr using for sound input.  just usb cams have shitter ranges
    //so 50 works
    //float range=200;
    
    fftAssignValues();
    
    parametersAssign();
    
    framebuffer0.begin();
    shader_mixer.begin();
    
    
    //videoGrabber.getTextureReference().draw(0, 0, 320, 640);
    if(scaleswitch==0){
        if(inputswitch==0){
            //videoGrabber.draw(0,0,320,240);
        }
        if(inputswitch==1){
            cam1.draw(0,0,320,240);
        }
    }
    if(scaleswitch==1){
        if(inputswitch==0){
            //videoGrabber.draw(0,0);
        }
        if(inputswitch==1){
            if(hdmi_aspect_ratio_switch==0){
                cam1.draw(0,0,640,480);
            }
            if(hdmi_aspect_ratio_switch==1){
                aspect_fix_fbo.draw(0,0,640,480);
            }
        }
    }
    /*
     //textures
     shader_mixer.setUniformTexture("fb0", pastFrames[d_delay].getTexture(),1);
     
     if(wet_dry_switch==1){
     shader_mixer.setUniformTexture("fb1", pastFrames[(abs(framedelayoffset-framebufferLength)-1)%framebufferLength].getTexture(),2);
     }
     if(wet_dry_switch==0){
     shader_mixer.setUniformTexture("fb1", dry_framebuffer.getTexture(),2);
     }
     //continuous variables
     shader_mixer.setUniform1f("fb0_lumakey_value",d_lumakey_value);
     shader_mixer.setUniform1f("fb0_mix",d_mix);
     shader_mixer.setUniform3f("fb0_hsbx",ofVec3f(d_hue,d_sat,d_bright));
     shader_mixer.setUniform1f("temporalFilterMix",d_temporalFilterMix);
     shader_mixer.setUniform1f("fb1_brightx",d_temporalFilterResonance );
     shader_mixer.setUniform1f("cam1_brightx",d_cam1_x);
     shader_mixer.setUniform1f("fb0_xdisplace",d_x);
     shader_mixer.setUniform1f("fb0_ydisplace",d_y);
     shader_mixer.setUniform1f("fb0_zdisplace",d_z);
     shader_mixer.setUniform1f("fb0_rotate",d_rotate);
     shader_mixer.setUniform3f("fb0_huex",ofVec3f(d_huex_mod,d_huex_off,d_huex_lfo));
     
     if(wet_dry_switch==1){
     shader_mixer.setUniform1i("toroidSwitch",toroidSwitch);
     shader_mixer.setUniform1i("mirrorSwitch",0);
     }
     
     if(wet_dry_switch==0){
     shader_mixer.setUniform1i("toroidSwitch",0);
     shader_mixer.setUniform1i("mirrorSwitch",toroidSwitch);
     }
     
     
     shader_mixer.setUniform1i("brightInvert",brightInvert);
     shader_mixer.setUniform1i("hueInvert",hueInvert);
     shader_mixer.setUniform1i("saturationInvert",saturationInvert);
     
     shader_mixer.setUniform1i("horizontalMirror",horizontalMirror);
     shader_mixer.setUniform1i("verticalMirror",verticalMirror);
     
     
     shader_mixer.setUniform1i("lumakeyInvertSwitch",lumakeyInvertSwitch);
     */
    //send the textures
    shader_mixer.setUniformTexture("fb", pastFrames[d_delay].getTexture(),1);
    if(wet_dry_switch==1){
        shader_mixer.setUniformTexture("temporalFilter", pastFrames[(abs(framedelayoffset-framebufferLength)-1)%framebufferLength].getTexture(),2);
    }
    if(wet_dry_switch==0){
        shader_mixer.setUniformTexture("temporalFilter", dry_framebuffer.getTexture(),2);
    }
    //send the continuous variables
    shader_mixer.setUniform1f("lumakey",d_lumakey_value);
    shader_mixer.setUniform1f("fbMix",d_mix);
    shader_mixer.setUniform1f("fbHue",d_hue);
    shader_mixer.setUniform1f("fbSaturation",d_sat);
    shader_mixer.setUniform1f("fbBright",d_bright);
    shader_mixer.setUniform1f("temporalFilterMix",d_temporalFilterMix);
    shader_mixer.setUniform1f("temporalFilterResonance",d_temporalFilterResonance );
    shader_mixer.setUniform1f("fbXDisplace",d_x);
    shader_mixer.setUniform1f("fbYDisplace",d_y);
    shader_mixer.setUniform1f("fbZDisplace",d_z);
    shader_mixer.setUniform1f("fbRotate",d_rotate);
    shader_mixer.setUniform3f("fb_huex",ofVec3f(d_huex_mod,d_huex_off,d_huex_lfo));
    shader_mixer.setUniform1f("fbHuexMod",d_huex_mod);
    shader_mixer.setUniform1f("fbHuexOff",d_huex_off);
    shader_mixer.setUniform1f("fbHuexLfo",d_huex_lfo);
    
    //send the switches
    shader_mixer.setUniform1i("toroidSwitch",toroidSwitch);
    shader_mixer.setUniform1i("mirrorSwitch",mirrorSwitch);
    shader_mixer.setUniform1i("brightInvert",brightInvert);
    shader_mixer.setUniform1i("hueInvert",hueInvert);
    shader_mixer.setUniform1i("saturationInvert",saturationInvert);
    shader_mixer.setUniform1i("horizontalMirror",horizontalMirror);
    shader_mixer.setUniform1i("verticalMirror",verticalMirror);
    shader_mixer.setUniform1i("lumakeyInvertSwitch",lumakeyInvertSwitch);
    
    //send the videoreactive controls
    shader_mixer.setUniform1f("vLumakey",vLumakeyValue*c_lumakey_value);
    shader_mixer.setUniform1f("vMix",vMix*c_mix);
    shader_mixer.setUniform1f("vHue",2.0f*vHue*c_hue);
    shader_mixer.setUniform1f("vSat",2.0f*vSaturation*c_sat);
    shader_mixer.setUniform1f("vBright",2.0f*vBright*c_bright);
    shader_mixer.setUniform1f("vtemporalFilterMix",vTemporalFilterMix*c_temporalFilterMix);
    shader_mixer.setUniform1f("vFb1X",vTemporalFilterResonance);
    shader_mixer.setUniform1f("vX",.01*vX);
    shader_mixer.setUniform1f("vY",.01*vY);
    shader_mixer.setUniform1f("vZ",vZ*c_z);
    shader_mixer.setUniform1f("vRotate",vRotate*c_rotate);
    shader_mixer.setUniform1f("vHuexMod",1.0f-vHuexMod);
    shader_mixer.setUniform1f("vHuexOff",vHuexOff*c_huex_off);
    shader_mixer.setUniform1f("vHuexLfo",vHuexLfo*c_huex_lfo);
    shader_mixer.end();
    framebuffer0.end();
    
    /**sharpening mode — bypass when amount is zero**/
    bool doSharpen = (d_sharpenAmount > 0.01f) || (vSharpenAmount * c_sharpenAmount > 0.01f);
    ofFbo& outputFbo = doSharpen ? sharpenFramebuffer : framebuffer0;

    if(doSharpen) {
        sharpenFramebuffer.begin();
        shaderSharpen.begin();
        framebuffer0.draw(0,0);
        shaderSharpen.setUniform1f("sharpenAmount",d_sharpenAmount);
        shaderSharpen.setUniform1f("vSharpenAmount",vSharpenAmount*c_sharpenAmount);
        shaderSharpen.end();
        sharpenFramebuffer.end();
    }

    outputFbo.draw(0,0,ofGetWidth(),ofGetHeight());

    blackoutSmoothed += (blackoutAmount - blackoutSmoothed) * 0.08f;
    if(blackoutSmoothed > 0.004f){
        ofEnableAlphaBlending();
        ofSetColor(0, 0, 0, (int)(blackoutSmoothed * 255));
        ofDrawRectangle(0, 0, ofGetWidth(), ofGetHeight());
        ofSetColor(255, 255, 255, 255);
    }

    pastFrames[abs(framebufferLength-framedelayoffset)-1].begin(); //eeettt
    if(wet_dry_switch==0){
        if(inputswitch==0){
            //videoGrabber.draw(0,0,framebuffer0.getWidth(),framebuffer0.getHeight());
        }
        if(inputswitch==1){
            if(hdmi_aspect_ratio_switch==0){
                cam1.draw(0,0,640,480);
            }
            if(hdmi_aspect_ratio_switch==1){
                aspect_fix_fbo.draw(0,0,640,480);
            }
        }
        
        dry_framebuffer.begin();
        //framebuffer0.draw(0,0);
        outputFbo.draw(0,0);
        dry_framebuffer.end();

    }//endifwetdry0

    if(wet_dry_switch==1){
        //framebuffer0.draw(0,0);
        outputFbo.draw(0,0);
    }//endifwetdry1
    
    pastFrames[abs(framebufferLength-framedelayoffset)-1].end(); //eeettt
    incIndex();
    
    //p_lock biz
    if(p_lock_switch==1){
        p_lock_increment++;
        p_lock_increment=p_lock_increment%p_lock_size;
    }
    
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
    ofSetColor(255);
    string msg="fps="+ofToString(ofGetFrameRate(),2)+" clear switch"+ofToString(clear_switch,5);//+" z="+ofToString(az,5);
    //ofDrawBitmapString(msg,10,10);
}

//--------------------------------------------------------------
void ofApp::exit() {
    
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
void ofApp::fillPLockFromCurrent() {
    for(int i=0;i<p_lock_number;i++){
        for(int j=0;j<p_lock_size;j++){
            p_lock[i][j]=p_lock[i][p_lock_increment];
        }
    }
}

void ofApp::saveAudioPatch(int slot) {
    string path = "/home/pi/audiopatches/AUDIO" + ofToString(slot, 0, 2, '0') + ".TXT" ;
    ofFile file(path, ofFile::WriteOnly) ;
    if (!file.is_open()) return ;
    for (int i = 1 ; i <= 16 ; i++) {
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
            default: lv=mv=hv=0; break ;
        }
        file << "low" << i << "=" << lv << "\n" ;
        file << "mid" << i << "=" << mv << "\n" ;
        file << "high" << i << "=" << hv << "\n" ;
    }
    file << "react=" << audioReactiveControlSwitch << "\n" ;
    file.close() ;
}

void ofApp::loadAudioPatch(int slot) {
    string path = "/home/pi/audiopatches/AUDIO" + ofToString(slot, 0, 2, '0') + ".TXT" ;
    ofFile file(path, ofFile::ReadOnly) ;
    if (!file.is_open()) return ;
    while (!file.eof()) {
        string line ;
        getline(file, line) ;
        if (line.empty()) continue ;
        int eq = line.find('=') ;
        if (eq == string::npos) continue ;
        string key = line.substr(0, eq) ;
        float val = ofToFloat(line.substr(eq + 1)) ;
        if      (key=="low1")  lowC1=val ;
        else if (key=="low2")  lowC2=val ;
        else if (key=="low3")  lowC3=val ;
        else if (key=="low4")  lowC4=val ;
        else if (key=="low5")  lowC5=val ;
        else if (key=="low6")  lowC6=val ;
        else if (key=="low7")  lowC7=val ;
        else if (key=="low8")  lowC8=val ;
        else if (key=="low9")  lowC9=val ;
        else if (key=="low10") lowC10=val ;
        else if (key=="low11") lowC11=val ;
        else if (key=="low12") lowC12=val ;
        else if (key=="low13") lowC13=val ;
        else if (key=="low14") lowC14=val ;
        else if (key=="low15") lowC15=val ;
        else if (key=="low16") lowC16=val ;
        else if (key=="mid1")  midC1=val ;
        else if (key=="mid2")  midC2=val ;
        else if (key=="mid3")  midC3=val ;
        else if (key=="mid4")  midC4=val ;
        else if (key=="mid5")  midC5=val ;
        else if (key=="mid6")  midC6=val ;
        else if (key=="mid7")  midC7=val ;
        else if (key=="mid8")  midC8=val ;
        else if (key=="mid9")  midC9=val ;
        else if (key=="mid10") midC10=val ;
        else if (key=="mid11") midC11=val ;
        else if (key=="mid12") midC12=val ;
        else if (key=="mid13") midC13=val ;
        else if (key=="mid14") midC14=val ;
        else if (key=="mid15") midC15=val ;
        else if (key=="mid16") midC16=val ;
        else if (key=="high1")  highC1=val ;
        else if (key=="high2")  highC2=val ;
        else if (key=="high3")  highC3=val ;
        else if (key=="high4")  highC4=val ;
        else if (key=="high5")  highC5=val ;
        else if (key=="high6")  highC6=val ;
        else if (key=="high7")  highC7=val ;
        else if (key=="high8")  highC8=val ;
        else if (key=="high9")  highC9=val ;
        else if (key=="high10") highC10=val ;
        else if (key=="high11") highC11=val ;
        else if (key=="high12") highC12=val ;
        else if (key=="high13") highC13=val ;
        else if (key=="high14") highC14=val ;
        else if (key=="high15") highC15=val ;
        else if (key=="high16") highC16=val ;
        else if (key=="react")  audioReactiveControlSwitch=(int)val ;
    }
    file.close() ;
}

void ofApp::fftAssignValues(){
    fftLow=fft.getLowVal() * inputGain;
    fftLow=ofClamp(fftLow,1.0f,range);
    fftLow=fftLow/range;
    low_value_smoothed=fftLow*smoothing_rate+(1.0f-smoothing_rate)*low_value_smoothed;
    
    fftMid=fft.getMidVal() * inputGain;
    fftMid=ofClamp(fftMid,1.0f,range);
    fftMid=fftMid/range;
    mid_value_smoothed=fftMid*smoothing_rate+(1.0f-smoothing_rate)*mid_value_smoothed;
    
    fftHigh=fft.getHighVal() * inputGain;
    fftHigh=ofClamp(fftHigh,1.0f,range);
    fftHigh=fftHigh/range;
    high_value_smoothed=fftHigh*smoothing_rate+(1.0f-smoothing_rate)*high_value_smoothed;
}
//-------------------------------------------------------------
void ofApp::parametersAssign(){
    if(wet_dry_switch==0){
        c_hue=1.0;
        c_sat=1.0;
        c_bright=1.0;
        c_x=.1;
        c_y=.1;
        c_z=.5;
    }
    
    d_delay=(abs(framedelayoffset-framebufferLength-fb0_delayamount-
                 int((p_lock_smoothed[15]+lowC16*low_value_smoothed+midC16*mid_value_smoothed+highC16*high_value_smoothed)*(framebufferLength-1.0))
                 )-1)%framebufferLength;
    d_lumakey_value=kk+c_lumakey_value*p_lock_smoothed[0]+4.0f*(
                                                                lowC1*low_value_smoothed+
                                                                midC1*mid_value_smoothed+
                                                                highC1*high_value_smoothed);
    d_mix=jm+c_mix*p_lock_smoothed[1]+4.0f*(
                                            lowC2*low_value_smoothed+
                                            midC2*mid_value_smoothed+
                                            highC2*high_value_smoothed);
    d_hue=fv*(1.0f+c_hue*p_lock_smoothed[2])+4.0f*(
                                                   lowC3*low_value_smoothed+
                                                   midC3*mid_value_smoothed+
                                                   highC3*high_value_smoothed);
    d_sat=gb*(1.0f+c_sat*p_lock_smoothed[3])+4.0f*(
                                                   lowC4*low_value_smoothed+
                                                   midC4*mid_value_smoothed+
                                                   highC4*high_value_smoothed);
    d_bright=hn*(1.0f+c_bright*p_lock_smoothed[4])+4.0f*(
                                                         lowC5*low_value_smoothed+
                                                         midC5*mid_value_smoothed+
                                                         highC5*high_value_smoothed);
    d_temporalFilterMix=op+c_temporalFilterMix*p_lock_smoothed[5]+4.0f*(
                                                                        lowC6*low_value_smoothed+
                                                                        midC6*mid_value_smoothed+
                                                                        highC6*high_value_smoothed);
    d_temporalFilterResonance=fb1_brightx+p_lock_smoothed[6]+4.0*(
                                                                  lowC7*low_value_smoothed+
                                                                  midC7*mid_value_smoothed+
                                                                  highC7*high_value_smoothed);
    d_sharpenAmount=ll+c_sharpenAmount*p_lock_smoothed[7]+8.0f*(
                                                                lowC8*low_value_smoothed+
                                                                midC8*mid_value_smoothed+
                                                                highC8*high_value_smoothed);
    d_x=sx+c_x*p_lock_smoothed[8]+4.0f*(
                                        c_x*(lowC9*low_value_smoothed+
                                             midC9*low_value_smoothed+
                                             highC9*high_value_smoothed));
    d_y=dc+c_y*p_lock_smoothed[9]+4.0f*(
                                        c_y*(lowC10*low_value_smoothed+
                                             midC10*mid_value_smoothed+
                                             highC10*high_value_smoothed));
    d_z=az*(1.0f+c_z*p_lock_smoothed[10])+4.0f*(
                                                c_z*(lowC11*low_value_smoothed+
                                                     midC11*mid_value_smoothed+
                                                     highC11*high_value_smoothed));
    d_rotate=qw+c_rotate*p_lock_smoothed[11]+4.0*(
                                                  c_rotate*(lowC12*low_value_smoothed+
                                                            midC12*mid_value_smoothed+
                                                            highC12*high_value_smoothed));
    d_huex_mod=er*(1.0f-p_lock_smoothed[12])+4.0f*(
                                                   lowC13*low_value_smoothed+
                                                   midC13*mid_value_smoothed+
                                                   highC13*high_value_smoothed);
    d_huex_off=ty+c_huex_off*p_lock_smoothed[13]+4.0f*(
                                                       c_huex_off*(lowC14*low_value_smoothed+
                                                                   midC14*mid_value_smoothed+
                                                                   highC14*high_value_smoothed));
    d_huex_lfo=ui+c_huex_lfo*p_lock_smoothed[14]+4.0f*(
                                                       c_huex_lfo*(lowC15*low_value_smoothed+
                                                                   midC15*mid_value_smoothed+
                                                                   highC15*high_value_smoothed));
}	
//--------------------------------------------------------------
void ofApp::newMidiMessage(ofxMidiMessage& msg) {
    // add the latest message to the message queue
    midiMessages.push_back(msg);
    // remove any old messages if we have too many
    while(midiMessages.size() > 64) {
        midiMessages.erase(midiMessages.begin());
    }
}
//------------------------------------------------------------
void ofApp::p_lockUpdate(){
    for(int i=0;i<p_lock_number;i++){
        p_lock_smoothed[i]=p_lock[i][p_lock_increment]*(1.0f-p_lock_smooth)+p_lock_smoothed[i]*p_lock_smooth;
        if(abs(p_lock_smoothed[i])<.05){p_lock_smoothed[i]=0;}
    }
}
//-------------------------------------------------------------
void ofApp::inputUpdate(){
    if(inputswitch==1){
        cam1.update();
        //corner crop and stretch to preserve hd aspect ratio
        if(hdmi_aspect_ratio_switch==1){
            aspect_fix_fbo.begin();
            cam1.draw(0,0,853,480);
            aspect_fix_fbo.end();
        }
    }
    if(inputswitch==0){
        //omx_updates();
    }
}
//------------------------------------------------------------
void ofApp::p_lockClear(){
    for(int i=0;i<p_lock_number;i++){
        for(int j=0;j<p_lock_size;j++){
            p_lock[i][j]=0;
        }
    }
}
//------------------------------------------------------------
void ofApp::midiSetup(){
    // print input ports to console
    midiIn.listInPorts();
    
    // open port by number (you may need to change this)
    midiIn.openPort(1);
    //midiIn.openPort("IAC Pure Data In");	// by name
    //midiIn.openVirtualPort("ofxMidiIn Input"); // open a virtual port
    
    // don't ignore sysex, timing, & active sense messages,
    // these are ignored by default
    midiIn.ignoreTypes(false, false, false);
    
    // add ofApp as a listener
    midiIn.addListener(this);
    
    // print received messages to the console
    midiIn.setVerbose(true);
}
//-------------------------------------------------------------
void ofApp::fbDeclareAndAllocate(){
    framebuffer0.allocate(width,height);
    framebuffer0.begin();
    ofClear(0,0,0,255);
    framebuffer0.end();
    
    aspect_fix_fbo.allocate(width,height);
    aspect_fix_fbo.begin();
    ofClear(0,0,0,255);
    aspect_fix_fbo.end();
    
    dry_framebuffer.allocate(width,height);
    dry_framebuffer.begin();
    ofClear(0,0,0,255);
    dry_framebuffer.end();
    
    sharpenFramebuffer.allocate(width,height);
    sharpenFramebuffer.begin();
    ofClear(0,0,0,255);
    sharpenFramebuffer.end();
    
    for(int i=0;i<framebufferLength;i++){
        pastFrames[i].allocate(width, height);
        pastFrames[i].begin();
        ofClear(0,0,0,255);
        pastFrames[i].end();
    }
}
//-------------------------------------------------------------
void ofApp::inputSetup(){
    //pass in the settings and it will start the camera
    if(inputswitch==0){
        //videoGrabber.setup(settings);
    }
    if(inputswitch==1){
        cam1.listDevices() ;
        cam1.setDeviceID(0) ;
        cam1.setDesiredFrameRate(30);
        cam1.initGrabber(width,height);
    }
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
        fb0_delayamount=0;
        
        brightInvert=0;
        hueInvert=0;
        saturationInvert=0;
        
        verticalMirror=0;
        horizontalMirror=0;
        
        x_skew=0;
        y_skew=0;
        
        framebuffer0.begin();
        ofClear(0, 0, 0, 255);
        framebuffer0.end();
        
        for(int i=0;i<framebufferLength;i++){
            pastFrames[i].begin();
            ofClear(0,0,0,255);
            pastFrames[i].end();
        }//endifor
    }
}
//------------------------------------------------------------------
void ofApp::midibiz(){
    
    for(unsigned int i = 0; i < midiMessages.size(); ++i) {
        
        ofxMidiMessage &message = midiMessages[i];
        
        if(message.status < MIDI_SYSEX) {
            if(message.status == MIDI_CONTROL_CHANGE) {
                
                if(message.control==91){
                    if(message.value==127){
                        forceMidiWrite = true ;
                        forceMidiWriteTimer = ofGetElapsedTimeMillis() ;
                        midiLatchClear() ;
                        for(int i=0;i<p_lock_number;i++){
                            midiActiveFloat[i]=1 ;
                        }
                    }
                    if(message.value==0){
                        forceMidiWrite = false ;
                        forceMidiWriteTimer = 0 ;
                        fillPLockFromCurrent() ;
                        midiLatchClear() ;
                    }
                }
                if(message.control==92){
                    saveAudioPatch((int)message.value) ;
                }
                if(message.control==93){
                    loadAudioPatch((int)message.value) ;
                }
                // Safety timeout — auto-disable forceMidiWrite after 2s
                if(forceMidiWrite && ofGetElapsedTimeMillis() - forceMidiWriteTimer > 2000){
                    forceMidiWrite = false ;
                    forceMidiWriteTimer = 0 ;
                    fillPLockFromCurrent() ;
                    midiLatchClear() ;
                }
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
                if(message.control==55){
                    if(message.value==127){
                        p_lock_switch=1;
                        for(int i=0;i<p_lock_number;i++){
                            p_lock_smoothed[i]=0;
                            for(int j=0;j<p_lock_size;j++){
                                p_lock[i][j]=p_lock[i][p_lock_increment];
                            }
                        }
                    }
                    if(message.value==0){
                        p_lock_switch=0;
                    }
                }
                if(message.control==32){
                    if(message.value==127){ x_2=TRUE; }
                    if(message.value==0){ x_2=FALSE; }
                }
                if(message.control==48){
                    if(message.value==127){ x_5=TRUE; }
                    if(message.value==0){ x_5=FALSE; }
                }
                if(message.control==64){
                    if(message.value==127){ x_10=TRUE; }
                    if(message.value==0){ x_10=FALSE; }
                }
                if(message.control==33){
                    if(message.value==127){ y_2=TRUE; }
                    if(message.value==0){ y_2=FALSE; }
                }
                if(message.control==49){
                    if(message.value==127){ y_5=TRUE; }
                    if(message.value==0){ y_5=FALSE; }
                }
                if(message.control==65){
                    if(message.value==127){ y_10=TRUE; }
                    if(message.value==0){ y_10=FALSE; }
                }
                if(message.control==34){
                    if(message.value==127){ z_2=TRUE; }
                    if(message.value==0){ z_2=FALSE; }
                }
                if(message.control==50){
                    if(message.value==127){ z_5=TRUE; }
                    if(message.value==0){ z_5=FALSE; }
                }
                if(message.control==66){
                    if(message.value==127){ z_10=TRUE; }
                    if(message.value==0){ z_10=FALSE; }
                }
                if(message.control==35){
                    if(message.value==127){ r_2=TRUE; }
                    if(message.value==0){ r_2=FALSE; }
                }
                if(message.control==51){
                    if(message.value==127){ r_5=TRUE; }
                    if(message.value==0){ r_5=FALSE; }
                }
                if(message.control==67){
                    if(message.value==127){ r_10=TRUE; }
                    if(message.value==0){ r_10=FALSE; }
                }
                if(message.control==36){
                    if(message.value==127){ huexx_0=TRUE; }
                    if(message.value==0){ huexx_0=FALSE; }
                }
                if(message.control==52){
                    if(message.value==127){ huexx_1=TRUE; }
                    if(message.value==0){ huexx_1=FALSE; }
                }
                if(message.control==68){
                    if(message.value==127){ huexx_2=TRUE; }
                    if(message.value==0){ huexx_2=FALSE; }
                }
                if(message.control==46){
                    if(message.value==127){ toroidSwitch=TRUE; }
                    if(message.value==0){ toroidSwitch=FALSE; }
                }
                if(message.control==61){
                    if(message.value==127){ mirrorSwitch=TRUE; }
                    if(message.value==0){ mirrorSwitch=FALSE; }
                }
                if(y_skew_switch==TRUE){ y_skew+=.00001; }
                if(x_skew_switch==TRUE){ x_skew+=.00001; }
                if(message.control==71){
                    if(message.value==127){ wet_dry_switch=FALSE; }
                    if(message.value==0){ wet_dry_switch=TRUE; }
                }
                if(message.control==37){
                    if(message.value==127){ huexy_0=TRUE; }
                    if(message.value==0){ huexy_0=FALSE; }
                }
                if(message.control==53){
                    if(message.value==127){ huexy_1=TRUE; }
                    if(message.value==0){ huexy_1=FALSE; }
                }
                if(message.control==69){
                    if(message.value==127){ huexy_2=TRUE; }
                    if(message.value==0){ huexy_2=FALSE; }
                }
                if(message.control==38){
                    if(message.value==127){ huexz_0=TRUE; }
                    if(message.value==0){ huexz_0=FALSE; }
                }
                if(message.control==54){
                    if(message.value==127){ huexz_1=TRUE; }
                    if(message.value==0){ huexz_1=FALSE; }
                }
                if(message.control==70){
                    if(message.value==127){ huexz_2=TRUE; }
                    if(message.value==0){ huexz_2=FALSE; }
                }
                if(message.control==60){
                    if(message.value==127){ lumakeyInvertSwitch=TRUE; }
                    if(message.value==0){ lumakeyInvertSwitch=FALSE; }
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
                        if(p_lock_0_switch==1){
                            vmidiActiveFloat[0]=0;
                            if(forceMidiWrite || abs(message.value/127.0f-p_lock[0][p_lock_increment])<CONTROL_THRESHOLD){
                                midiActiveFloat[0]=1;
                            }
                            if(midiActiveFloat[0]==1){
                                p_lock[0][p_lock_increment]=message.value/127.0f;
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
                        if(p_lock_0_switch==1){
                            vmidiActiveFloat[1]=0;
                            if(forceMidiWrite || abs((message.value-MIDI_MAGIC)/MIDI_MAGIC-p_lock[1][p_lock_increment])<CONTROL_THRESHOLD){
                                midiActiveFloat[1]=1;
                            }
                            if(midiActiveFloat[1]==1){
                                p_lock[1][p_lock_increment]=(message.value-MIDI_MAGIC)/MIDI_MAGIC;
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
                        if(p_lock_0_switch==1){
                            vmidiActiveFloat[2]=0;
                            if(forceMidiWrite || abs((message.value-MIDI_MAGIC)/MIDI_MAGIC-p_lock[2][p_lock_increment])<CONTROL_THRESHOLD){
                                midiActiveFloat[2]=1;
                            }
                            if(midiActiveFloat[2]==1){
                                p_lock[2][p_lock_increment]=(message.value-MIDI_MAGIC)/MIDI_MAGIC;
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
                        if(p_lock_0_switch==1){
                            vmidiActiveFloat[3]=0;
                            if(forceMidiWrite || abs((message.value-MIDI_MAGIC)/MIDI_MAGIC-p_lock[3][p_lock_increment])<CONTROL_THRESHOLD){
                                midiActiveFloat[3]=1;
                            }
                            if(midiActiveFloat[3]==1){
                                p_lock[3][p_lock_increment]=(message.value-MIDI_MAGIC)/MIDI_MAGIC;
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
                        if(p_lock_0_switch==1){
                            vmidiActiveFloat[4]=0;
                            if(forceMidiWrite || abs((message.value-MIDI_MAGIC)/MIDI_MAGIC-p_lock[4][p_lock_increment])<CONTROL_THRESHOLD){
                                midiActiveFloat[4]=1;
                            }
                            if(midiActiveFloat[4]==1){
                                p_lock[4][p_lock_increment]=(message.value-MIDI_MAGIC)/MIDI_MAGIC;
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
                        if(p_lock_0_switch==1){
                            vmidiActiveFloat[5]=0;
                            if(forceMidiWrite || abs((message.value-MIDI_MAGIC)/MIDI_MAGIC-p_lock[5][p_lock_increment])<CONTROL_THRESHOLD){
                                midiActiveFloat[5]=1;
                            }
                            if(midiActiveFloat[5]==1){
                                p_lock[5][p_lock_increment]=(message.value-MIDI_MAGIC)/MIDI_MAGIC;
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
                        if(p_lock_0_switch==1){
                            vmidiActiveFloat[6]=0;
                            if(forceMidiWrite || abs(message.value/127.0f-p_lock[6][p_lock_increment])<CONTROL_THRESHOLD){
                                midiActiveFloat[6]=1;
                            }
                            if(midiActiveFloat[6]==1){
                                p_lock[6][p_lock_increment]=message.value/127.0f;
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
                        if(p_lock_0_switch==1){
                            vmidiActiveFloat[7]=0;
                            if(forceMidiWrite || abs(message.value/127.0f-p_lock[7][p_lock_increment])<CONTROL_THRESHOLD){
                                midiActiveFloat[7]=1;
                            }
                            if(midiActiveFloat[7]==1){
                                p_lock[7][p_lock_increment]=message.value/127.0f;
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
                        if(p_lock_0_switch==1){
                            vmidiActiveFloat[8]=0;
                            { float xRaw=(message.value-MIDI_MAGIC)/MIDI_MAGIC; float xMul=(x_2?2.0f:x_5?5.0f:x_10?10.0f:1.0f);
                            if(forceMidiWrite || abs(xRaw*xMul-p_lock[8][p_lock_increment])<CONTROL_THRESHOLD*xMul){ midiActiveFloat[8]=1; }
                            if(midiActiveFloat[8]==1){ p_lock[8][p_lock_increment]=xRaw*xMul; } }
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
                            if(x_2==TRUE){ lowC9=2.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(x_5==TRUE){ lowC9=5.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(x_10==TRUE){ lowC9=10.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
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
                            if(x_2==TRUE){ midC9=2.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(x_5==TRUE){ midC9=5.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(x_10==TRUE){ midC9=10.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
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
                            if(x_2==TRUE){ highC9=2.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(x_5==TRUE){ highC9=5.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(x_10==TRUE){ highC9=10.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                        }
                    }
                }

                // CC121 — y displace (bipolar)
                if(message.control==121){
                    if(audioReactiveControlSwitch==0){
                        midiLowActiveFloat[9]=0;
                        midiMidActiveFloat[9]=0;
                        midiHighActiveFloat[9]=0;
                        if(p_lock_0_switch==1){
                            vmidiActiveFloat[9]=0;
                            { float yRaw=(message.value-MIDI_MAGIC)/MIDI_MAGIC; float yMul=(y_2?2.0f:y_5?5.0f:y_10?10.0f:1.0f);
                            if(forceMidiWrite || abs(yRaw*yMul-p_lock[9][p_lock_increment])<CONTROL_THRESHOLD*yMul){ midiActiveFloat[9]=1; }
                            if(midiActiveFloat[9]==1){ p_lock[9][p_lock_increment]=yRaw*yMul; } }
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
                            if(y_2==TRUE){ lowC10=2.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(y_5==TRUE){ lowC10=5.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(y_10==TRUE){ lowC10=10.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
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
                            if(y_2==TRUE){ midC10=2.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(y_5==TRUE){ midC10=5.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(y_10==TRUE){ midC10=10.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
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
                            if(y_2==TRUE){ highC10=2.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(y_5==TRUE){ highC10=5.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(y_10==TRUE){ highC10=10.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                        }
                    }
                }

                // CC122 — z displace (bipolar)
                if(message.control==122){
                    if(audioReactiveControlSwitch==0){
                        midiLowActiveFloat[10]=0;
                        midiMidActiveFloat[10]=0;
                        midiHighActiveFloat[10]=0;
                        if(p_lock_0_switch==1){
                            vmidiActiveFloat[10]=0;
                            { float zRaw=(message.value-MIDI_MAGIC)/MIDI_MAGIC; float zMul=(z_2?2.0f:z_5?5.0f:z_10?10.0f:1.0f);
                            if(forceMidiWrite || abs(zRaw*zMul-p_lock[10][p_lock_increment])<CONTROL_THRESHOLD*zMul){ midiActiveFloat[10]=1; }
                            if(midiActiveFloat[10]==1){ p_lock[10][p_lock_increment]=zRaw*zMul; } }
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
                            if(z_2==TRUE){ lowC11=2.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(z_5==TRUE){ lowC11=5.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(z_10==TRUE){ lowC11=10.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
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
                            if(z_2==TRUE){ midC11=2.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(z_5==TRUE){ midC11=5.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(z_10==TRUE){ midC11=10.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
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
                            if(z_2==TRUE){ highC11=2.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(z_5==TRUE){ highC11=5.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(z_10==TRUE){ highC11=10.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                        }
                    }
                }

                // CC123 — rotation (bipolar)
                if(message.control==123){
                    if(audioReactiveControlSwitch==0){
                        midiLowActiveFloat[11]=0;
                        midiMidActiveFloat[11]=0;
                        midiHighActiveFloat[11]=0;
                        if(p_lock_0_switch==1){
                            vmidiActiveFloat[11]=0;
                            { float rRaw=(message.value-MIDI_MAGIC)/MIDI_MAGIC; float rMul=(r_2?2.0f:r_5?5.0f:r_10?10.0f:1.0f);
                            if(forceMidiWrite || abs(rRaw*rMul-p_lock[11][p_lock_increment])<CONTROL_THRESHOLD*rMul){ midiActiveFloat[11]=1; }
                            if(midiActiveFloat[11]==1){ p_lock[11][p_lock_increment]=rRaw*rMul; } }
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
                            if(r_2==TRUE){ lowC12=2.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(r_5==TRUE){ lowC12=5.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(r_10==TRUE){ lowC12=10.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
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
                            if(r_2==TRUE){ midC12=2.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(r_5==TRUE){ midC12=5.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(r_10==TRUE){ midC12=10.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
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
                            if(r_2==TRUE){ highC12=2.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(r_5==TRUE){ highC12=5.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(r_10==TRUE){ highC12=10.0*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                        }
                    }
                }

                // CC124 — hue modulo (unipolar)
                if(message.control==124){
                    if(audioReactiveControlSwitch==0){
                        midiLowActiveFloat[12]=0;
                        midiMidActiveFloat[12]=0;
                        midiHighActiveFloat[12]=0;
                        if(p_lock_0_switch==1){
                            vmidiActiveFloat[12]=0;
                            { float hxxDiv=(huexx_0?64.0f:huexx_1?96.0f:huexx_2?127.0f:32.0f); float hxxVal=message.value/hxxDiv;
                            if(forceMidiWrite || abs(hxxVal-p_lock[12][p_lock_increment])<CONTROL_THRESHOLD*127.0f/hxxDiv){ midiActiveFloat[12]=1; }
                            if(midiActiveFloat[12]==1){ p_lock[12][p_lock_increment]=hxxVal; } }
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
                            if(huexx_0==TRUE){ lowC13=message.value/64.0f; }
                            if(huexx_1==TRUE){ lowC13=message.value/96.0f; }
                            if(huexx_2==TRUE){ lowC13=message.value/127.0f; }
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
                            if(huexx_0==TRUE){ midC13=message.value/64.0f; }
                            if(huexx_1==TRUE){ midC13=message.value/96.0f; }
                            if(huexx_2==TRUE){ midC13=message.value/127.0f; }
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
                            if(huexx_0==TRUE){ highC13=message.value/64.0f; }
                            if(huexx_1==TRUE){ highC13=message.value/96.0f; }
                            if(huexx_2==TRUE){ highC13=message.value/127.0f; }
                        }
                    }
                }

                // CC125 — hue offset (bipolar)
                if(message.control==125){
                    if(audioReactiveControlSwitch==0){
                        midiLowActiveFloat[13]=0;
                        midiMidActiveFloat[13]=0;
                        midiHighActiveFloat[13]=0;
                        if(p_lock_0_switch==1){
                            vmidiActiveFloat[13]=0;
                            { float hxyRaw=(message.value-MIDI_MAGIC)/MIDI_MAGIC; float hxyMul=(huexy_0?2.0f:huexy_1?4.0f:huexy_2?8.0f:1.0f);
                            if(forceMidiWrite || abs(hxyRaw*hxyMul-p_lock[13][p_lock_increment])<CONTROL_THRESHOLD*hxyMul){ midiActiveFloat[13]=1; }
                            if(midiActiveFloat[13]==1){ p_lock[13][p_lock_increment]=hxyRaw*hxyMul; } }
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
                            if(huexy_0==TRUE){ lowC14=2.0f*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(huexy_1==TRUE){ lowC14=4.0f*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(huexy_2==TRUE){ lowC14=8.0f*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
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
                            if(huexy_0==TRUE){ midC14=2.0f*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(huexy_1==TRUE){ midC14=4.0f*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(huexy_2==TRUE){ midC14=8.0f*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
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
                            if(huexy_0==TRUE){ highC14=2.0f*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(huexy_1==TRUE){ highC14=4.0f*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(huexy_2==TRUE){ highC14=8.0f*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                        }
                    }
                }

                // CC126 — hue lfo (bipolar)
                if(message.control==126){
                    if(audioReactiveControlSwitch==0){
                        midiLowActiveFloat[14]=0;
                        midiMidActiveFloat[14]=0;
                        midiHighActiveFloat[14]=0;
                        if(p_lock_0_switch==1){
                            vmidiActiveFloat[14]=0;
                            { float hxzRaw=(message.value-MIDI_MAGIC)/MIDI_MAGIC; float hxzMul=(huexz_0?2.0f:huexz_1?4.0f:huexz_2?8.0f:1.0f);
                            if(forceMidiWrite || abs(hxzRaw*hxzMul-p_lock[14][p_lock_increment])<CONTROL_THRESHOLD*hxzMul){ midiActiveFloat[14]=1; }
                            if(midiActiveFloat[14]==1){ p_lock[14][p_lock_increment]=hxzRaw*hxzMul; } }
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
                            if(huexz_0==TRUE){ lowC15=2.0f*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(huexz_1==TRUE){ lowC15=4.0f*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(huexz_2==TRUE){ lowC15=8.0f*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
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
                            if(huexz_0==TRUE){ midC15=2.0f*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(huexz_1==TRUE){ midC15=4.0f*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(huexz_2==TRUE){ midC15=8.0f*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
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
                            if(huexz_0==TRUE){ highC15=2.0f*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(huexz_1==TRUE){ highC15=4.0f*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                            if(huexz_2==TRUE){ highC15=8.0f*(message.value-MIDI_MAGIC)/MIDI_MAGIC; }
                        }
                    }
                }

                // CC127 — delay (unipolar, no video reactive)
                if(message.control==127){
                    if(audioReactiveControlSwitch==0){
                        midiLowActiveFloat[15]=0;
                        midiMidActiveFloat[15]=0;
                        midiHighActiveFloat[15]=0;
                        if(p_lock_0_switch==1){
                            if(forceMidiWrite || abs(message.value/127.0f-p_lock[15][p_lock_increment])<CONTROL_THRESHOLD){
                                midiActiveFloat[15]=1;
                            }
                            if(midiActiveFloat[15]==1){
                                p_lock[15][p_lock_increment]=(message.value)/127.0;
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
                    if(message.value==127){ brightInvert=TRUE; }
                    if(message.value==0){ brightInvert=FALSE; }
                }
                if(message.control==61){
                    if(message.value==127){ saturationInvert=TRUE; }
                    if(message.value==0){ saturationInvert=FALSE; }
                }
                if(message.control==62){
                    if(message.value==127){ hueInvert=TRUE; }
                    if(message.value==0){ hueInvert=FALSE; }
                }
                if(message.control==90){
                    if(message.value==127){ hdmi_aspect_ratio_switch=TRUE; }
                    if(message.value==0){ hdmi_aspect_ratio_switch=FALSE; }
                }
                if(message.control==41){
                    if(message.value==127){ horizontalMirror=TRUE; }
                    if(message.value==0){ horizontalMirror=FALSE; }
                }
                if(message.control==45){
                    if(message.value==127){ verticalMirror=TRUE; }
                    if(message.value==0){ verticalMirror=FALSE; }
                }
                if(message.control==59){
                    if(message.value==127){
                        vLumakeyValue=0.0; vMix=0.0; vHue=0.0; vSaturation=0.0;
                        vBright=0.0; vTemporalFilterMix=0.0; vTemporalFilterResonance=0.0;
                        vSharpenAmount=0.0; vX=0.0; vY=0.0; vZ=0.0; vRotate=0.0;
                        vHuexMod=0.0; vHuexOff=0.0; vHuexLfo=0.0;
                        lowC1=lowC2=lowC3=lowC4=lowC5=lowC6=lowC7=lowC8=lowC9=lowC10=lowC11=lowC12=lowC13=lowC14=lowC15=lowC16=0.0f;
                        midC1=midC2=midC3=midC4=midC5=midC6=midC7=midC8=midC9=midC10=midC11=midC12=midC13=midC14=midC15=midC16=0.0f;
                        highC1=highC2=highC3=highC4=highC5=highC6=highC7=highC8=highC9=highC10=highC11=highC12=highC13=highC14=highC15=highC16=0.0f;
                        for(int i=0;i<p_lock_number;i++){
                            midiActiveFloat[i]=0; vmidiActiveFloat[i]=0;
                            midiLowActiveFloat[i]=0; midiMidActiveFloat[i]=0; midiHighActiveFloat[i]=0;
                            for(int j=0;j<p_lock_size;j++){ p_lock[i][j]=0; }
                        }
                    }
                }
                if(message.control==58){
                    if(message.value==127){
                        if(clear_switch==0){
                            clear_switch=1;
                            framebuffer0.begin(); ofClear(0,0,0,255); framebuffer0.end();
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
                        p_lock_0_switch=0;
                    }
                    if(message.value==0){
                        p_lock_0_switch=1;
                    }
                }
                if(message.control==55){
                    if(message.value==127){
                        p_lock_switch=1;
                    }
                    if(message.value==0){
                        p_lock_switch=0;
                    }
                }
                if(message.control==58){
                    if(message.value==127){
                        for(int i=0;i<p_lock_number;i++){
                            for(int j=0;j<p_lock_size;j++){
                                p_lock[i][j]=0;
                            }
                        }
                    }
                }
                if(message.control==32){
                    if(message.value==127){
                        x_2=TRUE;
                    }
                    if(message.value==0){
                        x_2=FALSE;
                    }
                }
                if(message.control==48){
                    if(message.value==127){
                        x_5=TRUE;
                    }
                    if(message.value==0){
                        x_5=FALSE;
                    }
                }
                if(message.control==64){
                    if(message.value==127){
                        x_10=TRUE;
                    }
                    
                    if(message.value==0){
                        x_10=FALSE;
                    }
                }
                
                
                
                if(message.control==33){
                    if(message.value==127){
                        y_2=TRUE;
                    }
                    
                    if(message.value==0){
                        y_2=FALSE;
                    }
                }
                
                if(message.control==49){
                    if(message.value==127){
                        y_5=TRUE;
                    }
                    
                    if(message.value==0){
                        y_5=FALSE;
                    }
                }
                
                if(message.control==65){
                    if(message.value==127){
                        y_10=TRUE;
                    }
                    
                    if(message.value==0){
                        y_10=FALSE;
                    }
                }
                
                
                
                if(message.control==34){
                    if(message.value==127){
                        z_2=TRUE;
                    }
                    
                    if(message.value==0){
                        z_2=FALSE;
                    }
                }
                
                if(message.control==50){
                    if(message.value==127){
                        z_5=TRUE;
                    }
                    
                    if(message.value==0){
                        z_5=FALSE;
                    }
                }
                
                if(message.control==66){
                    if(message.value==127){
                        z_10=TRUE;
                    }
                    
                    if(message.value==0){
                        z_10=FALSE;
                    }
                }
                
                
                
                
                if(message.control==35){
                    if(message.value==127){
                        r_2=TRUE;
                    }
                    
                    if(message.value==0){
                        r_2=FALSE;
                    }
                }
                
                if(message.control==51){
                    if(message.value==127){
                        r_5=TRUE;
                    }
                    
                    if(message.value==0){
                        r_5=FALSE;
                    }
                }
                
                if(message.control==67){
                    if(message.value==127){
                        r_10=TRUE;
                    }
                    
                    if(message.value==0){
                        r_10=FALSE;
                    }
                }
                
                
                
                
                if(message.control==36){
                    if(message.value==127){
                        huexx_0=TRUE;
                    }
                    
                    if(message.value==0){
                        huexx_0=FALSE;
                    }
                }
                
                if(message.control==52){
                    if(message.value==127){
                        huexx_1=TRUE;
                    }
                    
                    if(message.value==0){
                        huexx_1=FALSE;
                    }
                }
                
                if(message.control==68){
                    if(message.value==127){
                        huexx_2=TRUE;
                    }
                    
                    if(message.value==0){
                        huexx_2=FALSE;
                    }
                }
                
                if(message.control==46){
                    if(message.value==127){
                        toroidSwitch=TRUE;
                    }
                    
                    if(message.value==0){
                        toroidSwitch=FALSE;
                    }
                }
                
                
                
                
                if(y_skew_switch==TRUE){
                    y_skew+=.00001;
                }
                
                if(x_skew_switch==TRUE){
                    x_skew+=.00001;
                }
                
                
                if(message.control==71){
                    if(message.value==127){
                        wet_dry_switch=FALSE;
                    }
                    
                    if(message.value==0){
                        wet_dry_switch=TRUE;
                    }
                    
                }
                
                //---------------------
                
                
                
                if(message.control==37){
                    if(message.value==127){
                        huexy_0=TRUE;
                    }
                    
                    if(message.value==0){
                        huexy_0=FALSE;
                    }
                }
                
                if(message.control==53){
                    if(message.value==127){
                        huexy_1=TRUE;
                    }
                    
                    if(message.value==0){
                        huexy_1=FALSE;
                    }
                }
                
                if(message.control==69){
                    if(message.value==127){
                        huexy_2=TRUE;
                    }
                    
                    if(message.value==0){
                        huexy_2=FALSE;
                    }
                }
                
                
                //---------------------
                
                
                
                if(message.control==38){
                    if(message.value==127){
                        huexz_0=TRUE;
                    }
                    
                    if(message.value==0){
                        huexz_0=FALSE;
                    }
                }
                
                if(message.control==54){
                    if(message.value==127){
                        huexz_1=TRUE;
                    }
                    
                    if(message.value==0){
                        huexz_1=FALSE;
                    }
                }
                
                if(message.control==70){
                    if(message.value==127){
                        huexz_2=TRUE;
                    }
                    
                    if(message.value==0){
                        huexz_2=FALSE;
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
                        if(p_lock_0_switch==1){
                            p_lock[0][p_lock_increment]=message.value/127.0f;
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
                        if(p_lock_0_switch==1){
                            p_lock[1][p_lock_increment]=(message.value-63)/63.0f;
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
                        if(p_lock_0_switch==1){
                            p_lock[2][p_lock_increment]=(message.value-63)/63.0f;
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
                        if(p_lock_0_switch==1){
                            p_lock[3][p_lock_increment]=(message.value-63)/63.0f;
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
                        if(p_lock_0_switch==1){
                            p_lock[4][p_lock_increment]=(message.value-63)/63.0f;
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
                        if(p_lock_0_switch==1){
                            p_lock[5][p_lock_increment]=(message.value-63)/63.0f;
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
                        if(p_lock_0_switch==1){
                            p_lock[6][p_lock_increment]=(message.value)/127.0f;
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
                        if(p_lock_0_switch==1){
                            p_lock[7][p_lock_increment]=(message.value)/127.0f;
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
                        p_lock[8][p_lock_increment]=(message.value-63)/63.0f;
                        
                        if(x_2==TRUE){
                            p_lock[8][p_lock_increment]=2.0*(message.value-63.0)/63.0f;
                        }
                        
                        if(x_5==TRUE){
                            p_lock[8][p_lock_increment]=5.0*(message.value-63.0)/63.0f;
                        }
                        if(x_10==TRUE){
                            p_lock[8][p_lock_increment]=10.0*(message.value-63.0)/63.0f;
                        }
                    }
                    
                    if(audioReactiveControlSwitch==1){
                        lowC9=(message.value-63)/63.0f;
                        
                        if(x_2==TRUE){
                            lowC9=2.0*(message.value-63.0)/63.0f;
                        }
                        
                        if(x_5==TRUE){
                            lowC9=5.0*(message.value-63.0)/63.0f;
                        }
                        if(x_10==TRUE){
                            lowC9=10.0*(message.value-63.0)/63.0f;
                        }
                    }
                    
                    if(audioReactiveControlSwitch==2){
                        midC9=(message.value-63)/63.0f;
                        
                        if(x_2==TRUE){
                            midC9=2.0*(message.value-63.0)/63.0f;
                        }
                        
                        if(x_5==TRUE){
                            midC9=5.0*(message.value-63.0)/63.0f;
                        }
                        if(x_10==TRUE){
                            midC9=10.0*(message.value-63.0)/63.0f;
                        }
                    }
                    
                    if(audioReactiveControlSwitch==3){
                        highC9=(message.value-63)/63.0f;
                        
                        if(x_2==TRUE){
                            highC9=2.0*(message.value-63.0)/63.0f;
                        }
                        
                        if(x_5==TRUE){
                            highC9=5.0*(message.value-63.0)/63.0f;
                        }
                        if(x_10==TRUE){
                            highC9=10.0*(message.value-63.0)/63.0f;
                        }
                    }
                    
                }
                
                //c10 maps to fb0 y displace
                if(message.control==121){
                    if(audioReactiveControlSwitch==0){
                        p_lock[9][p_lock_increment]=(message.value-63)/63.0f;
                        
                        if(y_2==TRUE){
                            p_lock[9][p_lock_increment]=2.0*(message.value-63.0)/63.0f;
                        }
                        
                        if(y_5==TRUE){
                            p_lock[9][p_lock_increment]=5.0*(message.value-63.0)/63.0f;
                        }
                        if(y_10==TRUE){
                            p_lock[9][p_lock_increment]=10.0*(message.value-63.0)/63.0f;
                        }
                    }
                    
                    if(audioReactiveControlSwitch==1){
                        lowC10=(message.value-63)/63.0f;
                        
                        if(y_2==TRUE){
                            lowC10=2.0*(message.value-63.0)/63.0f;
                        }
                        
                        if(y_5==TRUE){
                            lowC10=5.0*(message.value-63.0)/63.0f;
                        }
                        if(y_10==TRUE){
                            lowC10=10.0*(message.value-63.0)/63.0f;
                        }
                    }
                    
                    if(audioReactiveControlSwitch==2){
                        midC10=(message.value-63)/63.0f;
                        
                        if(y_2==TRUE){
                            midC10=2.0*(message.value-63.0)/63.0f;
                        }
                        
                        if(y_5==TRUE){
                            midC10=5.0*(message.value-63.0)/63.0f;
                        }
                        if(y_10==TRUE){
                            midC10=10.0*(message.value-63.0)/63.0f;
                        }
                    }
                    
                    if(audioReactiveControlSwitch==3){
                        highC10=(message.value-63)/63.0f;
                        
                        if(y_2==TRUE){
                            highC10=2.0*(message.value-63.0)/63.0f;
                        }
                        
                        if(y_5==TRUE){
                            highC10=5.0*(message.value-63.0)/63.0f;
                        }
                        if(y_10==TRUE){
                            highC10=10.0*(message.value-63.0)/63.0f;
                        }
                    }
                    
                    
                    
                }
                
                
                if(message.control==122){
                    if(audioReactiveControlSwitch==0){
                        p_lock[10][p_lock_increment]=(message.value-63.0)/63.0f;
                        
                        if(z_2==TRUE){
                            p_lock[10][p_lock_increment]=2.0*(message.value-63.0)/63.0f;
                        }
                        
                        if(z_5==TRUE){
                            p_lock[10][p_lock_increment]=5.0*(message.value-63.0)/63.0f;
                        }
                        if(z_10==TRUE){
                            p_lock[10][p_lock_increment]=10.0*(message.value-63.0)/63.0f;
                        }
                    }
                    
                    if(audioReactiveControlSwitch==1){
                        lowC11=(message.value-63.0)/63.0f;
                        
                        if(z_2==TRUE){
                            lowC11=2.0*(message.value-63.0)/63.0f;
                        }
                        
                        if(z_5==TRUE){
                            lowC11=5.0*(message.value-63.0)/63.0f;
                        }
                        if(z_10==TRUE){
                            lowC11=10.0*(message.value-63.0)/63.0f;
                        }
                    }
                    
                    if(audioReactiveControlSwitch==2){
                        midC11=(message.value-63.0)/63.0f;
                        
                        if(z_2==TRUE){
                            midC11=2.0*(message.value-63.0)/63.0f;
                        }
                        
                        if(z_5==TRUE){
                            midC11=5.0*(message.value-63.0)/63.0f;
                        }
                        if(z_10==TRUE){
                            midC11=10.0*(message.value-63.0)/63.0f;
                        }
                    }
                    
                    if(audioReactiveControlSwitch==3){
                        highC11=(message.value-63.0)/63.0f;
                        
                        if(z_2==TRUE){
                            highC11=2.0*(message.value-63.0)/63.0f;
                        }
                        
                        if(z_5==TRUE){
                            highC11=5.0*(message.value-63.0)/63.0f;
                        }
                        if(z_10==TRUE){
                            highC11=10.0*(message.value-63.0)/63.0f;
                        }
                    }
                    
                }
                
                if(message.control==123){
                    if(audioReactiveControlSwitch==0){
                        p_lock[11][p_lock_increment]=(message.value-63)/63.0f;
                        
                        if(r_2==TRUE){
                            p_lock[11][p_lock_increment]=2*(message.value-63.0)/63.0f;
                        }
                        
                        if(r_5==TRUE){
                            p_lock[11][p_lock_increment]=4*(message.value-63.0)/63.0f;
                        }
                        if(r_10==TRUE){
                            p_lock[11][p_lock_increment]=8*(message.value-63.0)/63.0f;
                        }
                    }
                    
                    if(audioReactiveControlSwitch==1){
                        lowC12=(message.value-63)/63.0f;
                        
                        if(r_2==TRUE){
                            lowC12=2*(message.value-63.0)/63.0f;
                        }
                        
                        if(r_5==TRUE){
                            lowC12=4*(message.value-63.0)/63.0f;
                        }
                        if(r_10==TRUE){
                            lowC12=8*(message.value-63.0)/63.0f;
                        }
                    }
                    
                    if(audioReactiveControlSwitch==2){
                        midC12=(message.value-63)/63.0f;
                        
                        if(r_2==TRUE){
                            midC12=2*(message.value-63.0)/63.0f;
                        }
                        
                        if(r_5==TRUE){
                            midC12=4*(message.value-63.0)/63.0f;
                        }
                        if(r_10==TRUE){
                            midC12=8*(message.value-63.0)/63.0f;
                        }
                    }
                    
                    if(audioReactiveControlSwitch==3){
                        highC12=(message.value-63)/63.0f;
                        
                        if(r_2==TRUE){
                            highC12=2*(message.value-63.0)/63.0f;
                        }
                        
                        if(r_5==TRUE){
                            highC12=4*(message.value-63.0)/63.0f;
                        }
                        if(r_10==TRUE){
                            highC12=8*(message.value-63.0)/63.0f;
                        }
                    }
                    
                    
                }
                
                
                if(message.control==124){
                    if(audioReactiveControlSwitch==0){
                        p_lock[12][p_lock_increment]=(message.value)/32.0f;
                        
                        if(huexx_0==TRUE){
                            p_lock[12][p_lock_increment]=message.value/64.0f;
                        }
                        
                        if(huexx_1==TRUE){
                            p_lock[12][p_lock_increment]=message.value/96.0f;
                        }
                        if(huexx_2==TRUE){
                            p_lock[12][p_lock_increment]=message.value/127.0f;
                        }
                    }
                    
                    if(audioReactiveControlSwitch==1){
                        lowC13=(message.value)/32.0f;
                        
                        if(huexx_0==TRUE){
                            lowC13=message.value/64.0f;
                        }
                        
                        if(huexx_1==TRUE){
                            lowC13=message.value/96.0f;
                        }
                        if(huexx_2==TRUE){
                            lowC13=message.value/127.0f;
                        }
                    }
                    
                    if(audioReactiveControlSwitch==2){
                        midC13=(message.value)/32.0f;
                        
                        if(huexx_0==TRUE){
                            midC13=message.value/64.0f;
                        }
                        
                        if(huexx_1==TRUE){
                            midC13=message.value/96.0f;
                        }
                        if(huexx_2==TRUE){
                            midC13=message.value/127.0f;
                        }
                    }
                    
                    if(audioReactiveControlSwitch==3){
                        highC13=(message.value)/32.0f;
                        
                        if(huexx_0==TRUE){
                            highC13=message.value/64.0f;
                        }
                        
                        if(huexx_1==TRUE){
                            highC13=message.value/96.0f;
                        }
                        if(huexx_2==TRUE){
                            highC13=message.value/127.0f;
                        }
                    }
                }
                
                
                if(message.control==125){
                    if(audioReactiveControlSwitch==0){
                        p_lock[13][p_lock_increment]=(message.value-63.0)/63.0f;
                        
                        if(huexy_0==TRUE){
                            p_lock[13][p_lock_increment]=2*(message.value-63.0)/63.0f;
                        }
                        
                        if(huexy_1==TRUE){
                            p_lock[13][p_lock_increment]=4*(message.value-63.0)/63.0f;
                        }
                        if(huexy_2==TRUE){
                            p_lock[13][p_lock_increment]=8*(message.value-63.0)/63.0f;
                        }
                    }
                    
                    if(audioReactiveControlSwitch==1){
                        lowC14=(message.value-63.0)/63.0f;
                        
                        if(huexy_0==TRUE){
                            lowC14=2*(message.value-63.0)/63.0f;
                        }
                        
                        if(huexy_1==TRUE){
                            lowC14=4*(message.value-63.0)/63.0f;
                        }
                        if(huexy_2==TRUE){
                            lowC14=8*(message.value-63.0)/63.0f;
                        }
                    }
                    
                    if(audioReactiveControlSwitch==2){
                        midC14=(message.value-63.0)/63.0f;
                        
                        if(huexy_0==TRUE){
                            midC14=2*(message.value-63.0)/63.0f;
                        }
                        
                        if(huexy_1==TRUE){
                            midC14=4*(message.value-63.0)/63.0f;
                        }
                        if(huexy_2==TRUE){
                            midC14=8*(message.value-63.0)/63.0f;
                        }
                    }
                    
                    if(audioReactiveControlSwitch==3){
                        highC14=(message.value-63.0)/63.0f;
                        
                        if(huexy_0==TRUE){
                            highC14=2*(message.value-63.0)/63.0f;
                        }
                        
                        if(huexy_1==TRUE){
                            highC14=4*(message.value-63.0)/63.0f;
                        }
                        if(huexy_2==TRUE){
                            highC14=8*(message.value-63.0)/63.0f;
                        }
                    }
                    
                    
                    
                    
                }
                
                
                if(message.control==126){
                    if(audioReactiveControlSwitch==0){
                        p_lock[14][p_lock_increment]=(message.value-63.0)/63.0f;
                        
                        if(huexz_0==TRUE){
                            p_lock[14][p_lock_increment]=2*(message.value-63.0)/63.0f;
                        }
                        
                        if(huexz_1==TRUE){
                            p_lock[14][p_lock_increment]=4*(message.value-63.0)/63.0f;
                        }
                        if(huexz_2==TRUE){
                            p_lock[14][p_lock_increment]=8*(message.value-63.0)/63.0f;
                        }
                    }
                    
                    if(audioReactiveControlSwitch==1){
                        lowC15=(message.value-63.0)/63.0f;
                        
                        if(huexz_0==TRUE){
                            lowC15=2*(message.value-63.0)/63.0f;
                        }
                        
                        if(huexz_1==TRUE){
                            lowC15=4*(message.value-63.0)/63.0f;
                        }
                        if(huexz_2==TRUE){
                            lowC15=8*(message.value-63.0)/63.0f;
                        }
                    }
                    
                    if(audioReactiveControlSwitch==2){
                        midC15=(message.value-63.0)/63.0f;
                        
                        if(huexz_0==TRUE){
                            midC15=2*(message.value-63.0)/63.0f;
                        }
                        
                        if(huexz_1==TRUE){
                            midC15=4*(message.value-63.0)/63.0f;
                        }
                        if(huexz_2==TRUE){
                            midC15=8*(message.value-63.0)/63.0f;
                        }
                    }
                    
                    if(audioReactiveControlSwitch==3){
                        highC15=(message.value-63.0)/63.0f;
                        
                        if(huexz_0==TRUE){
                            highC15=2*(message.value-63.0)/63.0f;
                        }
                        
                        if(huexz_1==TRUE){
                            highC15=4*(message.value-63.0)/63.0f;
                        }
                        if(huexz_2==TRUE){
                            highC15=8*(message.value-63.0)/63.0f;
                        }
                    }
                    
                    
                }
                
                
                if(message.control==127){
                    if(audioReactiveControlSwitch==0){
                        p_lock[15][p_lock_increment]=(message.value)/127.0;
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
                        brightInvert=TRUE;
                    }
                    
                    if(message.value==0){
                        brightInvert=FALSE;
                    }
                }
                
                //cc44 maps to saturationInvert
                if(message.control==61){
                    if(message.value==127){
                        saturationInvert=TRUE;
                    }
                    
                    if(message.value==0){
                        saturationInvert=FALSE;
                    }
                }
                
                if(message.control==62){
                    if(message.value==127){
                        //hueInvert=TRUE;
                        hdmi_aspect_ratio_switch=TRUE;
                    }
                    
                    if(message.value==0){
                        //hueInvert=FALSE;
                        hdmi_aspect_ratio_switch=FALSE;
                    }
                }
                
                
                //cc41 maps to horizontalMirror
                if(message.control==41){
                    if(message.value==127){
                        horizontalMirror=TRUE;
                    }
                    
                    if(message.value==0){
                        horizontalMirror=FALSE;
                    }
                }
                //cc45 maps to horizontalMirror
                if(message.control==45){
                    if(message.value==127){
                        verticalMirror=TRUE;
                    }
                    
                    if(message.value==0){
                        verticalMirror=FALSE;
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
