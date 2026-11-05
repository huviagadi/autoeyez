# autowaaave Pi 5 Port Plan

**Created:** 2026-09-08
**Status:** READY FOR REVIEW (pending Fable review)
**Current Hardware:** Raspberry Pi 3B+ (armv6l)
**Target Hardware:** Raspberry Pi 5 (aarch64)

---

## Summary

Port the autowaaave openFrameworks video processor from Pi 3B+ to Pi 5. The main changes involve architecture (32-bit ARM → 64-bit ARM), display system (dispmanx → KMS/DRM), and openFrameworks rebuild.

---

## Key Architecture Differences

| Component | Pi 3B+ | Pi 5 |
|-----------|--------|------|
| Architecture | armv6l (32-bit) | aarch64 (64-bit) |
| GPU | VideoCore IV | VideoCore VII |
| Display API | dispmanx (legacy) | KMS/DRM |
| openFrameworks | linuxarmv6l | linuxaarch64 |
| OS | Raspberry Pi OS Lite 32-bit | Raspberry Pi OS Lite 64-bit |
| Memory | 1GB | 4GB/8GB |
| Boot config | /boot/config.txt | /boot/firmware/config.txt |

---

## Files to Transfer from Current autowaaave

**CRITICAL - SCP these when Pi is reachable:**

```bash
# Patched openFrameworks file (has shmsrc bypass)
scp admin@autowaaave.local:/home/pi/openFrameworks/libs/openFrameworks/video/ofGstVideoGrabber.cpp \
    ~/Claude/autoeyez/autowaaave/pi5_port/

# New shmsink receiver service
scp admin@autowaaave.local:/etc/systemd/system/stream_receive_shm.service \
    ~/Claude/autoeyez/autowaaave/pi5_port/

# Updated autowaaave service (with SHMSRC_PATH)
scp admin@autowaaave.local:/etc/systemd/system/autowaaave.service \
    ~/Claude/autoeyez/autowaaave/pi5_port/autowaaave.service.current

# Splash script (if restoring)
scp admin@autowaaave.local:/home/pi/splash.py \
    ~/Claude/autoeyez/autowaaave/pi5_port/
```

**Already on Mac:**
- ofApp.cpp, ofApp.h
- shadersES2/ (all shader files)
- video_bridge.py
- config.txt (needs modification for Pi 5)
- asound.conf

---

## Step-by-Step Port Process

### Phase 1: Flash OS

1. Use Raspberry Pi Imager:
   - **OS:** Raspberry Pi OS Lite (64-bit) — Bookworm
   - **Hostname:** autowaaave
   - **Username:** pi (or admin, match current)
   - **Password:** admin
   - **WiFi:** Your network (for initial setup)
   - **SSH:** Enabled

2. Boot and connect via SSH

### Phase 2: Install Dependencies

```bash
sudo apt update && sudo apt upgrade -y

# Core build tools
sudo apt install -y git build-essential cmake

# OpenGL/Mesa
sudo apt install -y libgl1-mesa-dev libglu1-mesa-dev libglew-dev

# X11/windowing (even for headless GL)
sudo apt install -y libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxxf86vm-dev

# Audio
sudo apt install -y libasound2-dev libpulse-dev

# GStreamer (for shmsrc/shmsink)
sudo apt install -y gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad gstreamer1.0-libav libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev

# Python (for video_bridge.py)
sudo apt install -y python3-serial python3-mido

# MIDI
sudo apt install -y libasound2-dev
```

### Phase 3: Install openFrameworks for aarch64

```bash
cd ~
# Download 64-bit ARM version (0.12.0 or latest with aarch64 support)
wget https://github.com/openframeworks/openFrameworks/releases/download/0.12.0/of_v0.12.0_linuxaarch64_release.tar.gz
tar xzf of_v0.12.0_linuxaarch64_release.tar.gz
mv of_v0.12.0_linuxaarch64_release openFrameworks

# Run dependency installer
cd openFrameworks/scripts/linux/debian
sudo ./install_dependencies.sh
cd ~/openFrameworks/scripts/linux
./compileOF.sh -j4
```

### Phase 4: Apply shmsrc Bypass Patch

The patched `ofGstVideoGrabber.cpp` adds shmsrc bypass for shared memory video input (avoids v4l2loopback issues).

```bash
# Copy patched file (after SCP from old Pi or from Mac)
cp ~/patched_files/ofGstVideoGrabber.cpp \
   ~/openFrameworks/libs/openFrameworks/video/ofGstVideoGrabber.cpp

# Rebuild openFrameworks library
cd ~/openFrameworks/scripts/linux
./compileOF.sh -j4
```

**Patch contents** (add at start of `bool ofGstVideoGrabber::setup(int w, int h)`):

```cpp
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
```

Also add includes at top of file:
```cpp
#include <cstdlib>
#include <cstring>
```

### Phase 5: Create and Build AUTO_WAAAVE_4_5

```bash
cd ~/openFrameworks/apps/myApps
mkdir AUTO_WAAAVE_4_5
cd AUTO_WAAAVE_4_5

# Copy source files (from Mac via SCP)
# ofApp.cpp, ofApp.h

# Create bin/data/shadersES2 and copy shaders
mkdir -p bin/data/shadersES2
# Copy all .frag and .vert files

# Build
make -j4
```

### Phase 6: Configure Boot (Pi 5 specific)

**IMPORTANT:** Pi 5 uses `/boot/firmware/config.txt` not `/boot/config.txt`

Create `/boot/firmware/config.txt`:

```ini
# Pi 5 config for autowaaave

# HiFiBerry DAC+ ADC
dtoverlay=hifiberry-dacplusadc

# Disable onboard audio
dtparam=audio=off

# GPU memory (Pi 5 handles this differently but still useful)
gpu_mem=256

# HDMI settings
hdmi_force_hotplug=1

# Force specific resolution
# Pi 5 uses KMS, so mode is set via cmdline.txt or via wlr/drm
# For 720p:
# framebuffer_width=1280
# framebuffer_height=720

# Disable splash for faster boot
disable_splash=1

# I2C for any future expansion
dtparam=i2c_arm=on
```

**Note:** Pi 5 KMS/DRM handles HDMI differently. The `hdmi_group` and `hdmi_mode` settings may not work the same way. Test and adjust.

### Phase 7: Network Configuration

Edit `/etc/dhcpcd.conf` (or use NetworkManager on Bookworm):

```ini
interface eth0
static ip_address=10.0.0.1/24
```

### Phase 8: Install Services

Copy service files:

**stream_receive_shm.service:**
```ini
[Unit]
Description=Video Stream Receiver (shmsink)
After=network.target

[Service]
ExecStartPre=/bin/sleep 3
ExecStartPre=/bin/rm -f /tmp/video_shm
ExecStart=/usr/bin/gst-launch-1.0 \
    tcpserversrc host=0.0.0.0 port=1236 \
    ! tsdemux \
    ! h264parse \
    ! avdec_h264 \
    ! videoconvert \
    ! video/x-raw,format=RGB,width=720,height=480 \
    ! shmsink socket-path=/tmp/video_shm wait-for-connection=false shm-size=20000000
Restart=always
RestartSec=5
User=pi

[Install]
WantedBy=multi-user.target
```

**autowaaave.service:**
```ini
[Unit]
Description=AUTO_WAAAVE_4_5
After=stream_receive_shm.service
Requires=stream_receive_shm.service

[Service]
Type=simple
User=pi
WorkingDirectory=/home/pi/openFrameworks/apps/myApps/AUTO_WAAAVE_4_5
Environment=SHMSRC_PATH=/tmp/video_shm
ExecStart=/home/pi/openFrameworks/apps/myApps/AUTO_WAAAVE_4_5/bin/AUTO_WAAAVE_4_5
Restart=always
RestartSec=2
StartLimitIntervalSec=0

[Install]
WantedBy=multi-user.target
```

**video_bridge.service:**
```ini
[Unit]
Description=Video Bridge (Teensy serial)
After=autowaaave.service

[Service]
Type=simple
User=pi
ExecStart=/usr/bin/python3 /home/pi/video_bridge.py
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

Install:
```bash
sudo cp *.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable stream_receive_shm autowaaave video_bridge
```

### Phase 9: Audio Configuration (HiFiBerry)

Create `/etc/asound.conf`:
```
pcm.!default {
    type hw
    card 0
}
ctl.!default {
    type hw
    card 0
}
```

Verify after reboot:
```bash
arecord -l  # Should show HiFiBerry
aplay -l    # Should show HiFiBerry
```

### Phase 10: Test and Verify

```bash
# Reboot
sudo reboot

# Check services
systemctl status stream_receive_shm autowaaave video_bridge

# Check shm socket exists (when stream_receive_shm is running)
ls -la /tmp/video_shm

# Check logs
journalctl -u autowaaave -f
journalctl -u stream_receive_shm -f
```

---

## Potential Issues and Mitigations

### 1. OpenGL/EGL Differences

**Issue:** Pi 5 uses KMS/DRM exclusively; dispmanx is deprecated.

**Mitigation:** openFrameworks 0.12.0 should handle this. If issues arise:
- Ensure `dtoverlay=vc4-kms-v3d` is in config.txt (usually default)
- Check OF window/display initialization

### 2. GStreamer Version Differences

**Issue:** Pi 5 Bookworm has newer GStreamer than Pi 3B+ Buster.

**Mitigation:** Test shmsrc/shmsink pipeline manually first:
```bash
# Sender test
gst-launch-1.0 videotestsrc ! video/x-raw,format=RGB,width=720,height=480 ! shmsink socket-path=/tmp/test_shm

# Receiver test (another terminal)
gst-launch-1.0 shmsrc socket-path=/tmp/test_shm is-live=true ! video/x-raw,format=RGB,width=720,height=480 ! videoconvert ! autovideosink
```

### 3. HiFiBerry Compatibility

**Issue:** DAC+ ADC overlay might need adjustment for Pi 5.

**Mitigation:** Check HiFiBerry Pi 5 documentation. The overlay name may be different or need additional parameters.

### 4. Performance

**Expectation:** Pi 5 is significantly faster than Pi 3B+. Should handle shaders easily.

**Monitoring:**
```bash
vcgencmd measure_temp
htop
```

### 5. HDMI Output Resolution

**Issue:** KMS handles resolution differently.

**Mitigation:**
- Use `xrandr` to list/set modes if X is running
- For console/framebuffer mode, check `cmdline.txt` for video= parameter
- May need to adjust OF window creation code for fullscreen

---

## Code Changes Required

### ofApp.cpp — Resolution/Display

Current code assumes 720x480. Check:
- `ofSetWindowShape()` calls
- FBO dimensions
- Shader viewport settings

Pi 5 may default to different resolution. May need to force window size or use native resolution and scale internally.

### ofApp.cpp — Video Input

Should work unchanged with shmsrc bypass. The SHMSRC_PATH environment variable triggers the bypass in ofGstVideoGrabber.cpp.

### Shaders (shadersES2/)

Should work unchanged. GLES 2.0 shaders are forward-compatible. Pi 5 supports GLES 3.1 but will run ES2 shaders fine.

---

## Migration Checklist

- [ ] Flash Pi 5 with 64-bit Raspberry Pi OS Lite
- [ ] Install all dependencies
- [ ] Download and compile openFrameworks 0.12.0 linuxaarch64
- [ ] Apply shmsrc bypass patch to ofGstVideoGrabber.cpp
- [ ] Rebuild openFrameworks
- [ ] Copy ofApp.cpp, ofApp.h, shadersES2/
- [ ] Build AUTO_WAAAVE_4_5
- [ ] Configure /boot/firmware/config.txt
- [ ] Configure static IP on eth0
- [ ] Install HiFiBerry overlay, create asound.conf
- [ ] Copy and enable all systemd services
- [ ] Copy video_bridge.py
- [ ] Test stream_receive_shm pipeline
- [ ] Test autowaaave app
- [ ] Test video_bridge with Teensy
- [ ] Verify audio input (FFT)
- [ ] Image SD card as backup

---

## Files Summary

**From Mac (already have):**
- ofApp.cpp
- ofApp.h
- shadersES2/*.frag, *.vert
- video_bridge.py
- asound.conf

**Need to SCP from current autowaaave:**
- ofGstVideoGrabber.cpp (patched)
- stream_receive_shm.service
- autowaaave.service (current with SHMSRC_PATH)
- splash.py (optional)

**Create fresh on Pi 5:**
- /boot/firmware/config.txt (modified for Pi 5)
- /etc/dhcpcd.conf (static IP)

---

## Reference Commands

```bash
# SCP files from Mac to Pi 5
scp ~/Claude/autoeyez/autowaaave/ofApp.cpp pi@autowaaave.local:~/openFrameworks/apps/myApps/AUTO_WAAAVE_4_5/src/
scp ~/Claude/autoeyez/autowaaave/ofApp.h pi@autowaaave.local:~/openFrameworks/apps/myApps/AUTO_WAAAVE_4_5/src/
scp -r ~/Claude/autoeyez/autowaaave/shadersES2 pi@autowaaave.local:~/openFrameworks/apps/myApps/AUTO_WAAAVE_4_5/bin/data/
scp ~/Claude/autoeyez/autowaaave/video_bridge.py pi@autowaaave.local:~/
scp ~/Claude/autoeyez/autowaaave/pi5_port/ofGstVideoGrabber.cpp pi@autowaaave.local:~/openFrameworks/libs/openFrameworks/video/
```
