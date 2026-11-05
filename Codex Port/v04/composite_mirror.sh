#!/bin/bash
set -euo pipefail

pidfile=/run/user/$(id -u)/autowaaave-composite-mirror.pid
log=/tmp/composite_mirror.log

running_pid() {
  [ -s "$pidfile" ] || return 1
  pid="$(cat "$pidfile")"
  kill -0 "$pid" 2>/dev/null || return 1
  return 0
}

stop_mirror() {
  if running_pid; then
    kill "$pid"
    for _ in 1 2 3 4 5; do
      kill -0 "$pid" 2>/dev/null || break
      sleep 0.2
    done
  fi
  rm -f "$pidfile"
}

start_mirror() {
  if running_pid; then return 0; fi
  export DISPLAY="${DISPLAY:-:0}"
  export XAUTHORITY="${XAUTHORITY:-/home/admin/.Xauthority}"
  screen_size="$(xrandr --current | awk '/^Screen 0:/{gsub(/,/, "", $10); print $8 "x" $10; exit}')"
  screen_width="${screen_size%x*}"
  screen_height="${screen_size#*x}"
  end_x=$((screen_width - 1))
  end_y=$((screen_height - 1))
  nohup gst-launch-1.0 -e \
    ximagesrc display-name=:0 use-damage=false show-pointer=false \
      startx=0 starty=0 endx="$end_x" endy="$end_y" \
    ! video/x-raw,framerate=30/1 \
    ! videoscale method=1 add-borders=false \
    ! videoconvert \
    ! video/x-raw,width=720,height=480,pixel-aspect-ratio=1/1,format=BGRx \
    ! kmssink driver-name=drm-rp1-vec connector-id=45 \
      force-modesetting=true render-rectangle="<0,0,720,480>" sync=false \
    >"$log" 2>&1 </dev/null &
  echo $! > "$pidfile"
  sleep 1
  if ! running_pid; then
    cat "$log" >&2
    rm -f "$pidfile"
    return 1
  fi
}

case "${1:-}" in
  start) start_mirror ;;
  stop) stop_mirror ;;
  restart) stop_mirror; start_mirror ;;
  status)
    if running_pid; then echo "active pid=$pid"; else echo inactive; exit 1; fi ;;
  *) echo "usage: $0 start|stop|restart|status" >&2; exit 2 ;;
esac
