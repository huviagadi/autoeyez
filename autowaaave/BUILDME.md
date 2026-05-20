# autowaaave — Hardware Build Guide

Raspberry Pi 3B+ setup with HiFiBerry audio and openFrameworks for the autoeyez shader processor.

## Bill of Materials

| Qty | Part | Specification | Notes |
|-----|------|---------------|-------|
| 1 | Raspberry Pi 3B+ | | Pi 4 may work but untested |
| 1 | HiFiBerry DAC+ ADC | Or DAC+ ADC Pro | Line input for FFT |
| 1 | microSD card | 16GB+ | OS + openFrameworks |
| 1 | Ethernet cable | Cat5e or better | Direct to autoclip |
| 1 | HDMI cable | | Output to display |
| 1 | 5V 3A power supply | Micro-USB | |
| 1 | HiFiBerry-compatible case | Optional | Needs audio jack access |

## HiFiBerry DAC+ ADC

The HiFiBerry provides line-level audio input for FFT analysis (audio-reactive parameters). It stacks directly on the Pi's GPIO header.

### Installation

1. Stack HiFiBerry on Pi GPIO header
2. Add to `/boot/config.txt`:
```
dtoverlay=hifiberry-dacplusadc
```
3. Create `/etc/asound.conf`:
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
4. Reboot

### Verify Audio

```bash
arecord -l  # Should show HiFiBerry card
aplay -l    # Should show HiFiBerry card
```

## Raspberry Pi Setup

### 1. Flash OS

Use Raspberry Pi Imager:
- **OS:** Raspberry Pi OS Lite (32-bit) — for Pi 3B+ compatibility
- **Hostname:** autowaaave
- **Username:** admin
- **Password:** admin
- **WiFi:** Your network (for initial setup)
- **SSH:** Enabled

### 2. Static IP Configuration

Edit `/etc/dhcpcd.conf`:
```
interface eth0
static ip_address=10.0.0.1/24
```

### 3. Install Dependencies

```bash
sudo apt update
sudo apt install -y git build-essential cmake libgl1-mesa-dev \
    libglu1-mesa-dev libasound2-dev libpulse-dev libxrandr-dev \
    libxinerama-dev libxcursor-dev libxi-dev libxxf86vm-dev \
    v4l2loopback-dkms python3-serial python3-mido
```

### 4. Install openFrameworks

```bash
cd ~
wget https://github.com/openframeworks/openFrameworks/releases/download/0.11.2/of_v0.11.2_linuxarmv6l_release.tar.gz
tar xzf of_v0.11.2_linuxarmv6l_release.tar.gz
mv of_v0.11.2_linuxarmv6l_release openFrameworks
cd openFrameworks/scripts/linux/debian
sudo ./install_dependencies.sh
```

### 5. Create Project

```bash
cd ~/openFrameworks/apps/myApps
mkdir AUTO_WAAAVE_4_5
cd AUTO_WAAAVE_4_5
```

Copy `ofApp.cpp` and `ofApp.h` here.

### 6. Build

```bash
cd ~/openFrameworks/apps/myApps/AUTO_WAAAVE_4_5
make -j4
```

### 7. Setup v4l2loopback

The video stream from autoclip needs a virtual video device:

```bash
sudo modprobe v4l2loopback devices=1 video_nr=10 card_label="autoclip" exclusive_caps=1 max_buffers=2
```

Add to `/etc/modules-load.d/v4l2loopback.conf`:
```
v4l2loopback
```

Add to `/etc/modprobe.d/v4l2loopback.conf`:
```
options v4l2loopback devices=1 video_nr=10 card_label="autoclip" exclusive_caps=1 max_buffers=2
```

### 8. Install Services

```bash
sudo cp autowaaave.service stream_receive.service video_bridge.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable stream_receive autowaaave video_bridge
```

### 9. Copy Bridge Script

```bash
cp video_bridge.py /home/admin/
```

### 10. Create Audio Patches Directory

```bash
mkdir -p /home/admin/audiopatches
```

### 11. Reboot

```bash
sudo reboot
```

## Boot Configuration

Add to `/boot/config.txt`:

```
# HiFiBerry DAC+ ADC
dtoverlay=hifiberry-dacplusadc

# Disable onboard audio (conflicts with HiFiBerry)
dtparam=audio=off

# GPU memory for video processing
gpu_mem=256

# HDMI settings (adjust as needed)
hdmi_force_hotplug=1
hdmi_group=1
hdmi_mode=4  # 720p
```

## Network Configuration

autowaaave is the network hub between autoclip and automidi:

| Connection | Purpose |
|------------|---------|
| Ethernet → autoclip | Video stream (TCP:1236), commands (UDP) |
| USB → automidi (Teensy) | MIDI + serial |

**Static IP:** 10.0.0.1

## USB Connections

| Device | Purpose |
|--------|---------|
| /dev/ttyACM0 | Teensy (automidi) serial |
| USB MIDI | Teensy (automidi) MIDI CC |

The video_bridge.py script handles serial communication with the Teensy.

## Audio Input

Connect line-level audio to the HiFiBerry's RCA input jacks. This audio is analyzed via FFT for audio-reactive parameter modulation.

Typical sources:
- Mixer aux send
- Audio interface output
- Synth output

## HDMI Output

Connect HDMI to your display or video mixer. The processed video appears here at 720×480 (upscaled to 720p).

For composite output (if needed), add to `/boot/config.txt`:
```
enable_tvout=1
sdtv_mode=0  # NTSC
```

## Verifying Installation

### Check Services

```bash
systemctl status stream_receive autowaaave video_bridge
```

### Check v4l2loopback

```bash
v4l2-ctl --list-devices
# Should show /dev/video10 as "autoclip"
```

### Check Audio

```bash
arecord -D hw:0,0 -f cd -d 5 test.wav
aplay test.wav
```

### Check Serial

```bash
ls /dev/ttyACM*
# Should show /dev/ttyACM0 when Teensy connected
```

## Troubleshooting

| Problem | Likely Cause | Solution |
|---------|--------------|----------|
| No video output | Stream not received | Check autoclip running, ethernet connected |
| Black screen | OF app crashed | Check `journalctl -u autowaaave` |
| No audio reactivity | HiFiBerry not detected | Check config.txt, asound.conf |
| MIDI not received | Wrong port | Check OF opens MIDI port 1 |
| Bridge not connecting | Teensy not detected | Check /dev/ttyACM0 exists |
| High latency | v4l2loopback misconfigured | Check exclusive_caps=1, max_buffers=2 |

## Performance Notes

- Pi 3B+ GPU handles 720×480 @ 30fps (test for frame drops)
- Monitor framerate if GPU struggles
- Shader complexity affects performance
- Monitor GPU temperature: `vcgencmd measure_temp`

## Deployment Script

The parent folder contains `deploy_autowaaave.sh` for updating the OF app:

```bash
./deploy_autowaaave.sh
```

This copies ofApp.cpp to the Pi, rebuilds, and restarts the service.
