#!/bin/bash
set -euo pipefail

GADGET=/sys/kernel/config/usb_gadget/autoeyez_midi_test
UDC=1000480000.usb

modprobe libcomposite

# Remove the legacy gadget if it was loaded manually or by an older boot setup.
if grep -q '^g_midi ' /proc/modules; then
    modprobe -r g_midi
fi

if [ ! -d "$GADGET" ]; then
    mkdir "$GADGET"
    echo 0x17b3 > "$GADGET/idVendor"
    echo 0x0004 > "$GADGET/idProduct"
    echo 0x0200 > "$GADGET/bcdUSB"
    echo 0x0612 > "$GADGET/bcdDevice"
    echo full-speed > "$GADGET/max_speed"

    mkdir "$GADGET/strings/0x409"
    echo 'Grey Innovation' > "$GADGET/strings/0x409/manufacturer"
    echo 'MIDI Gadget' > "$GADGET/strings/0x409/product"

    mkdir "$GADGET/configs/c.1"
    echo 0x80 > "$GADGET/configs/c.1/bmAttributes"
    echo 2 > "$GADGET/configs/c.1/MaxPower"
    mkdir "$GADGET/configs/c.1/strings/0x409"
    echo MIDI > "$GADGET/configs/c.1/strings/0x409/configuration"

    mkdir "$GADGET/functions/midi.usb0"
    echo 1 > "$GADGET/functions/midi.usb0/in_ports"
    echo 1 > "$GADGET/functions/midi.usb0/out_ports"
    echo 1024 > "$GADGET/functions/midi.usb0/qlen"
    echo 512 > "$GADGET/functions/midi.usb0/buflen"
    ln -s "$GADGET/functions/midi.usb0" "$GADGET/configs/c.1/midi.usb0"
fi

current_udc=$(cat "$GADGET/UDC")
if [ -z "$current_udc" ]; then
    echo "$UDC" > "$GADGET/UDC"
elif [ "$current_udc" != "$UDC" ]; then
    echo "Unexpected MIDI gadget UDC: $current_udc" >&2
    exit 1
fi
