# autoclip — Hardware Build Guide

Raspberry Pi 5 setup with composite video capture cards for the autoeyez mixer/player.

## Bill of Materials

| Qty | Part | Specification | Notes |
|-----|------|---------------|-------|
| 1 | Raspberry Pi 5 | 4GB or 8GB RAM | |
| 3 | USB composite capture card | MJPEG 640×480 capable | See compatibility |
| 1 | Powered USB hub | 4+ ports, 5V 2A+ | Powers capture cards |
| 1 | microSD card | 32GB+ | OS + clip storage |
| 1 | Ethernet cable | Cat5e or better | Direct to autowaaave |
| 1 | 5V 5A USB-C power supply | Official Pi 5 PSU recommended | |
| 1 | Case with ventilation | Optional | Pi 5 runs warm |

## Capture Card Compatibility

### Recommended
Generic "USB 2.0 Video Capture" cards with the following chipsets work well:
- **UTV007** — Most common, reliable MJPEG support
- **Somagic SMI-2021** — Works, may need configuration

Look for cards advertising:
- MJPEG capture (not just raw/YUV)
- 640×480 @ 30fps
- Linux/Mac compatibility (often unstated but usually works)

### Known Working
Search for "USB Video Capture Card Composite" — the small dongle-style cards (~$5-10 each) typically work. They appear as `/dev/video0`, `/dev/video2`, `/dev/video4` (even numbers, odd are metadata).

### Avoid
- Cards requiring proprietary drivers
- "HD" capture cards (different protocol, higher latency)
- Cards that only support raw YUV output (high bandwidth)

## Device Mapping

When three capture cards are connected:

| Device | Capture Card | Composite Input |
|--------|--------------|-----------------|
| /dev/video0 | Card 1 | COMPOSITE 1 |
| /dev/video2 | Card 2 | COMPOSITE 2 |
| /dev/video4 | Card 3 | COMPOSITE 3 |

Odd-numbered devices (/dev/video1, 3, 5) are metadata endpoints.

**Note:** Device assignment can change on reboot. For consistent mapping, use udev rules based on USB port or serial number.

## Raspberry Pi 5 Setup

### 1. Flash OS

Use Raspberry Pi Imager:
- **OS:** Raspberry Pi OS Lite (64-bit)
- **Hostname:** autoclip
- **Username:** admin
- **Password:** admin
- **WiFi:** Your network (for initial setup)
- **SSH:** Enabled

### 2. Static IP Configuration

Edit `/etc/dhcpcd.conf`:
```
interface eth0
static ip_address=10.0.0.2/24
```

### 3. Install Dependencies

```bash
sudo apt update
sudo apt install -y python3-opencv python3-numpy ffmpeg v4l-utils
```

### 4. Copy Software

```bash
scp mixer.py video_player.py video_control.py admin@autoclip.local:/home/admin/
scp video.service admin@autoclip.local:/tmp/
ssh admin@autoclip.local 'sudo mv /tmp/video.service /etc/systemd/system/'
```

### 5. Install Service

```bash
ssh admin@autoclip.local 'sudo systemctl daemon-reload && sudo systemctl enable video.service'
```

### 6. Create Clip Directory

```bash
ssh admin@autoclip.local 'sudo mkdir -p /boot/firmware/clips'
```

### 7. Add Clips

Copy MP4 files to `/boot/firmware/clips/`:
```bash
scp 01_myclip.mp4 admin@autoclip.local:/boot/firmware/clips/
```

Naming: `NN_NAME.mp4` — prefix determines sort order, name displayed on controller.

### 8. Reboot

```bash
ssh admin@autoclip.local 'sudo reboot'
```

## Clip Preparation

### Format Requirements
- **Container:** MP4
- **Codec:** H.264 (most compatible)
- **Resolution:** 720×480 or 640×480 (will be scaled)
- **Framerate:** 30fps recommended

### Transcoding Script

The parent folder contains `transcode_clip.sh` for converting clips:

```bash
./transcode_clip.sh input.mov
```

This creates a properly formatted MP4 in the same directory.

## USB Hub Recommendations

A powered hub is required to supply adequate current to 3 capture cards. Look for:
- Individual port power switching (helpful for debugging)
- 5V 2A+ total output
- USB 2.0 is sufficient (capture cards are USB 2.0)

Example setup:
```
Pi 5 USB-A port → Powered USB Hub → 3× Capture Cards
                                  → (optional) USB storage
```

## Verifying Capture Cards

Check detected devices:
```bash
v4l2-ctl --list-devices
```

Test capture:
```bash
ffmpeg -f v4l2 -input_format mjpeg -video_size 640x480 -i /dev/video0 -frames 1 test.jpg
```

List supported formats:
```bash
v4l2-ctl -d /dev/video0 --list-formats-ext
```

## Network Configuration

autoclip communicates with autowaaave via direct ethernet connection.

| Setting | Value |
|---------|-------|
| autoclip IP | 10.0.0.2 |
| autowaaave IP | 10.0.0.1 |
| Video stream | TCP port 1236 |
| Commands in | UDP port 5006 |
| Status out | UDP port 5005 |

No router needed — direct ethernet cable between the two Pis.

## Framebuffer Output

The mixer writes directly to `/dev/fb0` for optional local display. This is primarily for debugging — the main output goes via TCP stream to autowaaave.

To see local output, connect HDMI or use a composite output adapter.

## Troubleshooting

| Problem | Likely Cause | Solution |
|---------|--------------|----------|
| No capture devices | Cards not detected | Check USB hub power, try different ports |
| Wrong device numbers | USB enumeration order | Use udev rules or check dmesg |
| Dropped frames | USB bandwidth | Use powered hub, check for USB 3.0 conflicts |
| Stream disconnects | Network issue | Check ethernet cable, verify IPs |
| Clips won't play | Wrong format | Re-transcode with transcode_clip.sh |
| Service won't start | Permission issue | Service must run as root for framebuffer |

## Power Considerations

Pi 5 + 3 capture cards + USB hub can draw significant power. Use the official 27W Pi 5 power supply to avoid undervoltage.

Check for throttling:
```bash
vcgencmd get_throttled
```

`0x0` = OK, any other value indicates power issues.
