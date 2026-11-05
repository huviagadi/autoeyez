#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#ifdef __linux__
#include <linux/input.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#endif

// Independent, transient layer: never writes patches, p-locks or MIDI values.
class ControllerLayer {
public:
    struct Offsets { float x=0, y=0, rotation=0, zoom=0; } offsets;
    float deadzone = 0.12f;
    float xRange = 0.1f, yRange = 0.1f;
    float rotationRange = 0.314159265f, zoomRange = 0.5f;
    float smoothingSeconds = 0.04f;
    bool enabled = true;
    ControllerLayer() = default;
    ControllerLayer(const ControllerLayer&) = delete;
    ControllerLayer& operator=(const ControllerLayer&) = delete;
    ~ControllerLayer() { disconnect(); }

    static float normalize(int value, int minimum, int maximum) {
        if(maximum <= minimum) return 0;
        const double center = (double(minimum) + maximum) * 0.5;
        const double span = value >= center ? maximum-center : center-minimum;
        return float(std::max(-1.0, std::min(1.0, (value-center)/span)));
    }
    static void radialDeadzone(float& x, float& y, float deadzone) {
        const float length = std::sqrt(x*x + y*y);
        const float dz = std::max(0.0f, std::min(0.95f, deadzone));
        if(length <= dz) { x=y=0; return; }
        const float scale = (std::min(1.0f,length)-dz)/((1.0f-dz)*length);
        x*=scale; y*=scale;
    }
    void setSticks(float lx, float ly, float rx, float ry, float dt) {
        radialDeadzone(lx,ly,deadzone);
        radialDeadzone(rx,ry,deadzone);
        // Linux Y is positive down. Right-stick up decreases UV scale, visually zooming in.
        Offsets target;
        target.x=lx*xRange; target.y=ly*yRange;
        target.rotation=rx*rotationRange; target.zoom=ry*zoomRange;
        const float alpha = smoothingSeconds <= 0 ? 1.0f :
            1.0f-std::exp(-std::max(0.0f,std::min(0.1f,dt))/smoothingSeconds);
        offsets.x += alpha*(target.x-offsets.x);
        offsets.y += alpha*(target.y-offsets.y);
        offsets.rotation += alpha*(target.rotation-offsets.rotation);
        offsets.zoom += alpha*(target.zoom-offsets.zoom);
        // Settle to exact neutral; avoids an indefinitely decaying offset.
        if(target.x==0 && std::abs(offsets.x)<1e-6f) offsets.x=0;
        if(target.y==0 && std::abs(offsets.y)<1e-6f) offsets.y=0;
        if(target.rotation==0 && std::abs(offsets.rotation)<1e-6f) offsets.rotation=0;
        if(target.zoom==0 && std::abs(offsets.zoom)<1e-6f) offsets.zoom=0;
    }
    bool connected() const { return fd >= 0; }
    void update(uint64_t nowMs, float dt) {
        if(!enabled) { disconnect(); return; }
#ifdef __linux__
        if(fd < 0 && (!scanned || nowMs-lastScanMs >= 2000)) {
            scanned=true; lastScanMs=nowMs; discover();
        }
        if(fd < 0) return;
        // Read current kernel state directly: SYN_DROPPED cannot leave stale axes.
        // No exclusive grab and no event queue processing on the render thread.
        input_absinfo state[4] = {};
        for(int i=0;i<4;++i) {
            if(ioctl(fd,EVIOCGABS(axisCodes[i]),&state[i])<0) {
                std::cerr << "[controller] disconnected; offsets cleared\n";
                disconnect(); return;
            }
        }
        setSticks(normalize(state[0].value,state[0].minimum,state[0].maximum),
                  normalize(state[1].value,state[1].minimum,state[1].maximum),
                  normalize(state[2].value,state[2].minimum,state[2].maximum),
                  normalize(state[3].value,state[3].minimum,state[3].maximum),dt);
#else
        (void)nowMs; (void)dt;
#endif
    }
    void disconnect() {
#ifdef __linux__
        if(fd >= 0) close(fd);
#endif
        fd=-1; offsets=Offsets{};
    }
private:
    int fd=-1;
#ifdef __linux__
    uint64_t lastScanMs=0;
    bool scanned=false;
    const int axisCodes[4] = {ABS_X,ABS_Y,ABS_RX,ABS_RY};
    void discover() {
        DIR* directory=opendir("/dev/input");
        if(!directory) return;
        while(dirent* entry=readdir(directory)) {
            if(std::string(entry->d_name).compare(0,5,"event")!=0) continue;
            const std::string path=std::string("/dev/input/")+entry->d_name;
            int candidate=open(path.c_str(),O_RDONLY|O_NONBLOCK|O_CLOEXEC);
            if(candidate<0) continue;
            input_id id={};
            // Original Nintendo Switch Pro Controller, USB or Bluetooth.
            bool valid=ioctl(candidate,EVIOCGID,&id)>=0 &&
                       id.vendor==0x057e && id.product==0x2009;
            for(int i=0;valid && i<4;++i) {
                input_absinfo axis={};
                valid=ioctl(candidate,EVIOCGABS(axisCodes[i]),&axis)>=0 &&
                      axis.maximum>axis.minimum;
            }
            if(valid) {
                fd=candidate;
                std::cerr << "[controller] Switch Pro connected: " << path << '\n';
                break;
            }
            close(candidate);
        }
        closedir(directory);
    }
#endif
};
