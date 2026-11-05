// ============================================================
// SHMSRC BYPASS PATCH for ofGstVideoGrabber.cpp
// ============================================================
//
// This patch adds shared memory video input support to openFrameworks,
// bypassing v4l2loopback which has buffer allocation issues with appsink.
//
// HOW TO APPLY:
// 1. Open: ~/openFrameworks/libs/openFrameworks/video/ofGstVideoGrabber.cpp
// 2. Add the two #includes near the top (with other includes)
// 3. Add the bypass block at the VERY START of: bool ofGstVideoGrabber::setup(int w, int h)
// 4. Rebuild openFrameworks: cd ~/openFrameworks/scripts/linux && ./compileOF.sh -j4
// 5. Rebuild your app
//
// ============================================================

// ADD THESE INCLUDES near top of file (with other #includes):
// ============================================================
#include <cstdlib>   // for getenv()
#include <cstring>   // for strlen()


// ADD THIS BLOCK at the VERY START of:
// bool ofGstVideoGrabber::setup(int w, int h)
// ============================================================
// It must be BEFORE any device detection code.

/*
bool ofGstVideoGrabber::setup(int w, int h){
    // SHMSRC bypass: skip all device detection if SHMSRC_PATH is set
    const char* shmsrc_path = getenv("SHMSRC_PATH");
    if(shmsrc_path != NULL && strlen(shmsrc_path) > 0) {
        bIsCamera = true;
        char pipeline_str[512];
        snprintf(pipeline_str, sizeof(pipeline_str),
            "shmsrc socket-path=%s is-live=true ! video/x-raw,format=RGB,width=%d,height=%d,framerate=%d/1",
            shmsrc_path, w, h, attemptFramerate);
        ofLogNotice("ofGstVideoGrabber") << "Using shmsrc override: " << shmsrc_path;
        return videoUtils.setPipeline(string(pipeline_str), OF_PIXELS_RGB, false, w, h) && videoUtils.startPipeline();
    }

    // ... rest of original setup() function continues here ...
*/


// ============================================================
// USAGE:
// ============================================================
// Set environment variable before running OF app:
//   export SHMSRC_PATH=/tmp/video_shm
//
// Or in systemd service:
//   Environment=SHMSRC_PATH=/tmp/video_shm
//
// The shared memory socket must be created by a shmsink pipeline:
//   gst-launch-1.0 ... ! shmsink socket-path=/tmp/video_shm ...
//
// ============================================================
