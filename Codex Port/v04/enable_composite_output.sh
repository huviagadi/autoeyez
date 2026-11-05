#!/bin/bash
set -euo pipefail

config=/boot/firmware/config.txt
active_start=/home/admin/start_pi_5_stable_04.sh
ready_start=/home/admin/start_pi_5_stable_04.composite-ready.sh
stamp="$(date +%Y%m%d-%H%M%S)"

sudo cp "$config" "$config.pre-composite-$stamp"
cp "$active_start" "$active_start.pre-composite-$stamp"
sudo sed -i 's/dtoverlay=vc4-kms-v3d,cma-512$/dtoverlay=vc4-kms-v3d,cma-512,composite/' "$config"
cp "$ready_start" "$active_start"
chmod +x "$active_start" /home/admin/set_video_output.sh /home/admin/composite_mirror.sh
echo HDMI > /tmp/video_output_mode
echo "Composite overlay enabled. Reboot autowaaave to activate HDMI/COMP/BOTH switching."
