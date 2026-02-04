#!/usr/bin/env bash
# Move Anything shim entrypoint
# Runs MoveOriginal with the Move Anything shim, unless safe mode is triggered

SAFE_MODE_FILE="/data/UserData/move-anything/safe-mode"

# Check for safe mode trigger
if [ -f "$SAFE_MODE_FILE" ]; then
    # Remove the trigger file (one-shot)
    rm -f "$SAFE_MODE_FILE"
    # Log that we're entering safe mode
    echo "$(date): Safe mode triggered, running stock Move" >> /data/UserData/move-anything/safe-mode.log
    # Run stock Move without the shim
    exec /opt/move/MoveOriginal
fi

# Normal startup with shim
exec env LD_PRELOAD=move-anything-shim.so /opt/move/MoveOriginal
