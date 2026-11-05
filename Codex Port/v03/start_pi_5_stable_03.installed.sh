#!/bin/bash
# Launch the 1080p30 v03 integration build from an X session on the Pi 5.
sleep 2

xrandr --output HDMI-2 --mode 1920x1080 --rate 60 --primary 2>/dev/null
# HDMI-1 is the internal/secondary display. Keep it as a clone for now;
# its 59.99 Hz EDID mode is close enough to mirror HDMI-2's 60 Hz output.
xrandr --output HDMI-1 --mode 1920x1080 --rate 59.99 --same-as HDMI-2 2>/dev/null

# This installation runs unattended. Keep DPMS enabled long enough to force
# the DRM connector on, but give it no standby/suspend/off timeout.
xset s off 2>/dev/null
xset s noblank 2>/dev/null
xset +dpms 2>/dev/null
xset dpms 0 0 0 2>/dev/null
xset dpms force on 2>/dev/null

export MESA_GL_VERSION_OVERRIDE=2.1
export MESA_GLSL_VERSION_OVERRIDE=120

exec /home/admin/openFrameworks/apps/myApps/pi_5_stable_03/bin/pi_5_stable_03
