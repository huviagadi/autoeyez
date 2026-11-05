#!/bin/sh
set -eu
rm -f /run/systemd/system/stream_receive_shm.service.d/90-mosh-trial.conf
systemctl daemon-reload
systemctl restart stream_receive_shm.service
