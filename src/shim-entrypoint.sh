#!/usr/bin/env bash

# Start USB audio daemon if available (must run before Move for gadget setup)
# Gadget configfs requires root - use su since entrypoint runs as ableton
USB_AUDIO_SETUP="/data/UserData/move-anything/usb_audio/setup_gadget.sh"
if [ -x "$USB_AUDIO_SETUP" ]; then
    su -c "$USB_AUDIO_SETUP" root >/tmp/uac2_setup.log 2>&1 || true
fi

LD_PRELOAD=move-anything-shim.so /opt/move/MoveOriginal
