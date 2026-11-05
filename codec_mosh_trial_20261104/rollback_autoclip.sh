#!/bin/sh
set -eu
rm -f /run/systemd/system/video.service.d/90-mosh-trial.conf
systemctl daemon-reload
systemctl restart video.service
