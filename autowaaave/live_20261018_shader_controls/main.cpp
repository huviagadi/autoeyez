#include "ofMain.h"
#include "ofApp.h"
#include <fstream>
int main(){
    ofGLFWWindowSettings settings;
    settings.setSize(1920,1080);
    settings.windowMode=OF_FULLSCREEN;
    settings.setGLVersion(2,1);
    // Full-screen video quads have no interior geometry edges for MSAA to fix.
    settings.numSamples=0;
    std::ifstream sampleFile("/dev/shm/waaave_window_samples");
    int samples=0;
    if(sampleFile>>samples && samples==4) settings.numSamples=4;
    ofCreateWindow(settings);
    ofRunApp(new ofApp());
}
