#!/bin/bash
# Launch the 1080p30 v04 integration build from an X session on the Pi 5.
sleep 2

# Preferred low-latency audio analyzer. The 512-sample HiFiBerry path remains
# available at runtime through /home/admin/set_fft_source.sh PI.
echo TEENSY > /dev/shm/fft_source

# In the enclosure HDMI-1 drives the external 1080p output; HDMI-2 drives
# the internal panel. The projector's EDID exposes a nonstandard 172.78 MHz
# timing under the generic 1920x1080 name, so install the standard CEA
# 148.5 MHz 1080p60 timing explicitly.
xrandr --newmode 1080p60_CEA 148.50 1920 2008 2052 2200 1080 1084 1089 1125 +HSync +VSync 2>/dev/null || true
xrandr --addmode HDMI-1 1080p60_CEA 2>/dev/null || true
xrandr --output HDMI-1 --mode 1080p60_CEA --primary --set "Broadcast RGB" "Limited 16:235" 2>/dev/null
xrandr --output HDMI-2 --mode 1920x1080 --rate 60 --same-as HDMI-1 --scale 1x1 2>/dev/null

# Default to the internal panel plus external HDMI. The controller can switch
# the external connector to composite or drive both after startup.
/home/admin/set_video_output.sh HDMI || true

# This installation runs unattended. Keep DPMS enabled long enough to force
# the DRM connector on, but give it no standby/suspend/off timeout.
xset s off 2>/dev/null
xset s noblank 2>/dev/null
xset +dpms 2>/dev/null
xset dpms 0 0 0 2>/dev/null
xset dpms force on 2>/dev/null

export MESA_GL_VERSION_OVERRIDE=2.1
export MESA_GLSL_VERSION_OVERRIDE=120

exec /home/admin/openFrameworks/apps/myApps/pi_5_stable_04/bin/pi_5_stable_04
