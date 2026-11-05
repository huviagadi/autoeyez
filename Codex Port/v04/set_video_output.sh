#!/bin/bash
set -u

mode="${1:-HDMI}"
case "$mode" in HDMI|COMP|BOTH) ;; *) echo "usage: $0 HDMI|COMP|BOTH" >&2; exit 2 ;; esac

export DISPLAY="${DISPLAY:-:0}"
export XAUTHORITY="${XAUTHORITY:-/home/admin/.Xauthority}"
mirror=/home/admin/composite_mirror.sh

# The projector's EDID names a nonstandard PC timing 1920x1080. Use the
# standard CEA 148.5 MHz timing so it can lock reliably after mode changes.
xrandr --newmode 1080p60_CEA 148.50 1920 2008 2052 2200 1080 1084 1089 1125 +HSync +VSync 2>/dev/null || true
xrandr --addmode HDMI-1 1080p60_CEA 2>/dev/null || true

if [ "$mode" = HDMI ]; then
  "$mirror" stop
  xrandr --output HDMI-1 --mode 1080p60_CEA --primary --scale 1x1 --set "Broadcast RGB" "Limited 16:235"
  xrandr --output HDMI-2 --mode 1920x1080 --rate 60 --same-as HDMI-1 --scale 1x1
elif [ "$mode" = COMP ]; then
  "$mirror" stop
  xrandr --output HDMI-1 --off
  xrandr --output HDMI-2 --mode 1920x1080 --rate 60 --primary --scale 1x1
  "$mirror" start
else
  "$mirror" stop
  xrandr --output HDMI-1 --mode 1080p60_CEA --primary --scale 1x1 --set "Broadcast RGB" "Limited 16:235"
  xrandr --output HDMI-2 --mode 1920x1080 --rate 60 --same-as HDMI-1 --scale 1x1
  "$mirror" start
fi

printf '%s\n' "$mode" > /tmp/video_output_mode
echo "$mode"
