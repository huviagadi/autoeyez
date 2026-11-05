#!/bin/bash
# Launch the 1080p30 v03 integration build from an X session on the Pi 5.
sleep 2

# In the enclosure HDMI-1 drives the external 1080p output; HDMI-2 drives
# the internal panel. Drive both from the same unscaled 1080p framebuffer;
# scaling in the Pi compositor costs roughly 7 fps, whereas the panel's own
# scaler keeps the full render rate.
xrandr --output HDMI-1 --mode 1920x1080 --rate 60 --primary 2>/dev/null
xrandr --output HDMI-2 --mode 1920x1080 --rate 60 --same-as HDMI-1 --scale 1x1 2>/dev/null

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
