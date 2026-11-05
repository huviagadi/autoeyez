#!/bin/bash
set -euo pipefail
source_name="${1:-}"
case "$source_name" in
  PI|pi) source_name=PI ;;
  TEENSY|teensy) source_name=TEENSY ;;
  *) echo "usage: $0 PI|TEENSY" >&2; exit 2 ;;
esac
printf '%s\n' "$source_name" > /dev/shm/fft_source
echo "FFT source: $source_name"
