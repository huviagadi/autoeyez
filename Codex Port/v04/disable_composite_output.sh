#!/bin/bash
set -euo pipefail
sudo sed -i 's/dtoverlay=vc4-kms-v3d,cma-512,composite$/dtoverlay=vc4-kms-v3d,cma-512/' /boot/firmware/config.txt
echo "Composite overlay disabled. Reboot autowaaave to apply."
