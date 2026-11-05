#!/bin/bash
set -euo pipefail

mode="${1:-}"
seconds="${2:-30}"
case "$mode" in HDMI|COMP|BOTH) ;; *) echo "usage: $0 HDMI|COMP|BOTH [seconds]" >&2; exit 2 ;; esac

log=/tmp/pi_5_stable_04.log
start_lines="$(wc -l < "$log")"
/home/admin/set_video_output.sh "$mode"
echo "Measuring $mode for $seconds seconds..."
sleep "$seconds"

tail -n "+$((start_lines + 1))" "$log" | awk -v mode="$mode" '
  /PERF fps=/ {
    if (match($0, /fps=[0-9.]+/)) {
      value=substr($0, RSTART+4, RLENGTH-4) + 0
      sum += value
      count++
      if (count == 1 || value < min) min=value
      if (count == 1 || value > max) max=value
    }
  }
  END {
    if (count) printf "%s: avg %.2f fps, min %.2f, max %.2f (%d samples)\n", mode, sum/count, min, max, count
    else { print mode ": no PERF samples found"; exit 1 }
  }
'
