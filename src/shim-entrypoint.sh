#!/usr/bin/env bash

# Start audio stream daemon if available (no root required - uses UDP over NCM)
AUDIO_STREAM="/data/UserData/move-anything/usb_audio/audio_stream_daemon"
if [ -x "$AUDIO_STREAM" ]; then
    "$AUDIO_STREAM" -d >/tmp/audio_stream.log 2>&1 || true
fi

exec env LD_PRELOAD=move-anything-shim.so /opt/move/MoveOriginal
