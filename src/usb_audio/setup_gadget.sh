#!/bin/bash
# setup_gadget.sh - Production FunctionFS gadget setup for UAC2 daemon
#
# Called at boot before Move starts. Adds a FunctionFS function to the
# existing USB gadget, mounts it, starts the UAC2 daemon, and rebinds
# the UDC.
#
# The gadget must be unbound/rebound to add FunctionFS, which causes
# a brief USB disconnect. This is acceptable at boot time.

set -e

BASE_DIR="/data/UserData/move-anything"
GADGET_DIR="/sys/kernel/config/usb_gadget/g1"
FFS_MOUNT="/dev/uac2_ffs"
UDC_NAME="fe980000.usb"
DAEMON="$BASE_DIR/usb_audio/uac2_daemon"
PID_FILE="/var/run/uac2_daemon.pid"
CONFIG_FILE="$BASE_DIR/usb_audio.conf"

log() {
    echo "uac2_setup: $*"
}

# Check if USB audio is disabled via config
if [ -f "$CONFIG_FILE" ]; then
    if grep -q "^enabled=false" "$CONFIG_FILE" 2>/dev/null; then
        log "USB audio disabled in config, skipping"
        exit 0
    fi
fi

# Check if daemon binary exists
if [ ! -x "$DAEMON" ]; then
    log "daemon not found at $DAEMON, skipping USB audio setup"
    exit 0
fi

# Check if gadget exists
if [ ! -d "$GADGET_DIR" ]; then
    log "no gadget at $GADGET_DIR, skipping"
    exit 0
fi

# Check if already running
if [ -f "$PID_FILE" ]; then
    pid=$(cat "$PID_FILE" 2>/dev/null)
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        log "daemon already running (pid $pid)"
        exit 0
    fi
    rm -f "$PID_FILE"
fi

# Create FunctionFS function if needed
if [ ! -d "$GADGET_DIR/functions/ffs.uac2" ]; then
    log "adding FunctionFS function to gadget..."

    # Unbind UDC
    echo "" > "$GADGET_DIR/UDC" 2>/dev/null || true
    sleep 0.3

    # Create function and link to config
    mkdir -p "$GADGET_DIR/functions/ffs.uac2"
    ln -sf "$GADGET_DIR/functions/ffs.uac2" "$GADGET_DIR/configs/c.1/" 2>/dev/null || true
fi

# Mount FunctionFS
mkdir -p "$FFS_MOUNT"
if ! mountpoint -q "$FFS_MOUNT" 2>/dev/null; then
    mount -t functionfs uac2 "$FFS_MOUNT"
fi

# Start daemon (it writes descriptors to ep0 before UDC bind)
log "starting UAC2 daemon..."
"$DAEMON" -f "$FFS_MOUNT" -d

# Brief wait for daemon to write descriptors
sleep 0.5

# Rebind UDC (if it was unbound above)
current_udc=$(cat "$GADGET_DIR/UDC" 2>/dev/null)
if [ -z "$current_udc" ]; then
    log "rebinding UDC..."
    echo "$UDC_NAME" > "$GADGET_DIR/UDC"
fi

log "USB audio setup complete"
