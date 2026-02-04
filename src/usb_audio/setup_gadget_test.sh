#!/bin/bash
# setup_gadget_test.sh - Phase 0 FunctionFS gadget setup for validation
#
# Run on Move device as root BEFORE starting uac2_test.
# This adds a FunctionFS function to the existing gadget alongside NCM.
#
# WARNING: This briefly disconnects USB (unbind/rebind UDC), which will
# drop the NCM network connection momentarily. Run via serial console
# or from a local script.
#
# Usage:
#   ssh root@move 'bash -s' < setup_gadget_test.sh
#   # Then on device: ./uac2_test /dev/uac2_ffs

set -e

GADGET_DIR="/sys/kernel/config/usb_gadget/g1"
FFS_MOUNT="/dev/uac2_ffs"
UDC_NAME="fe980000.usb"

echo "=== Phase 0: FunctionFS Gadget Setup ==="

# Check if configfs gadget exists
if [ ! -d "$GADGET_DIR" ]; then
    echo "ERROR: No gadget found at $GADGET_DIR"
    echo "The USB gadget must already exist (Move creates it at boot)"
    exit 1
fi

# Check if ffs function already exists
if [ -d "$GADGET_DIR/functions/ffs.uac2" ]; then
    echo "FunctionFS function already exists"
    if mountpoint -q "$FFS_MOUNT" 2>/dev/null; then
        echo "Already mounted at $FFS_MOUNT"
        echo "Ready for uac2_test"
        exit 0
    fi
else
    echo "Creating FunctionFS function..."

    # Unbind UDC (brief USB disconnect)
    echo "Unbinding UDC (USB will disconnect briefly)..."
    echo "" > "$GADGET_DIR/UDC" 2>/dev/null || true
    sleep 0.5

    # Create the FunctionFS function
    mkdir -p "$GADGET_DIR/functions/ffs.uac2"

    # Link it into the configuration
    ln -sf "$GADGET_DIR/functions/ffs.uac2" "$GADGET_DIR/configs/c.1/"

    echo "FunctionFS function created and linked"
fi

# Mount FunctionFS
mkdir -p "$FFS_MOUNT"
if ! mountpoint -q "$FFS_MOUNT" 2>/dev/null; then
    echo "Mounting FunctionFS at $FFS_MOUNT..."
    mount -t functionfs uac2 "$FFS_MOUNT"
fi

echo ""
echo "=== Setup Complete ==="
echo ""
echo "Next steps:"
echo "  1. Run: ./uac2_test $FFS_MOUNT"
echo "     (this writes descriptors to ep0)"
echo "  2. Then bind UDC: echo '$UDC_NAME' > $GADGET_DIR/UDC"
echo "     (or uac2_test can prompt you)"
echo "  3. Check host: system_profiler SPAudioDataType (macOS)"
echo ""
echo "To tear down: umount $FFS_MOUNT && rmdir $FFS_MOUNT"
