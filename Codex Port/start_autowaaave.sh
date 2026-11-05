#!/bin/bash
# Wait for X to be ready
sleep 2

# Force single 1080p output on HDMI-2 (port nearest USB-C)
xrandr --output HDMI-1 --off 2>/dev/null
xrandr --output HDMI-2 --mode 1920x1080 --primary 2>/dev/null

# Mesa GL 2.1 compatibility context override
# Pi 5 Mesa defaults to GL 3.1+ core profile which breaks GLES2-style shaders.
# This forces a 2.1 context with GLSL 1.20, allowing the ES2 shaders to run
# without rewriting them for modern GL. Tested working 2026-09-14.
export MESA_GL_VERSION_OVERRIDE=2.1
export MESA_GLSL_VERSION_OVERRIDE=120

# Run the app
exec /home/admin/openFrameworks/apps/myApps/AUTO_WAAAVE_4_5/bin/AUTO_WAAAVE_4_5
