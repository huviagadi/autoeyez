#!/bin/sh
# Restore the original raw YUV420p wire path on both Pis.
set -eu
ssh -o BatchMode=yes admin@autowaaave.local '
  sudo rm -f /etc/systemd/system/stream_receive_shm.service.d/90-codec.conf
  sudo systemctl disable --now codec_damage.service
  sudo systemctl daemon-reload
  sudo systemctl restart stream_receive_shm.service
'
ssh -o BatchMode=yes admin@autoclip.local '
  sudo rm -f /etc/systemd/system/video.service.d/90-codec.conf
  sudo systemctl daemon-reload
  sudo systemctl restart video.service
'
