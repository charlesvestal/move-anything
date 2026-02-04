# Phase 1 Safety Guardrails Test Plan

## Overview

This document describes how to test the Phase 1 safety guardrails implementation:
- Safe mode (file-based boot bypass)
- Module crash handling (auto-unload after consecutive failures)
- Disabled modules list (persistence and management)

## Prerequisites

- Move device connected and accessible via SSH
- Latest build deployed: `./scripts/install.sh local --skip-modules`

## Test Script

```bash
#!/bin/bash
# Phase 1 Safety Guardrails Test Script

set -e
HOST="move.local"

echo "=== Phase 1 Safety Guardrails Tests ==="
echo

# Deploy first
echo "1. Deploying latest build..."
./scripts/install.sh local --skip-modules
echo

# Test: Create crashing module
echo "2. Creating crash test module..."
ssh ableton@$HOST 'mkdir -p /data/UserData/move-anything/modules/crashtest'
ssh ableton@$HOST 'cat > /data/UserData/move-anything/modules/crashtest/module.json << EOF
{"id": "crashtest", "name": "Crash Test", "standalone": true}
EOF'
ssh ableton@$HOST 'cat > /data/UserData/move-anything/modules/crashtest/ui.js << EOF
globalThis.init = function() { console.log("crashtest init"); };
globalThis.tick = function() { throw new Error("intentional crash"); };
EOF'
echo "   Created crashtest module"
echo

# Verify disabled list is empty
echo "3. Checking disabled modules list (should be empty)..."
ssh ableton@$HOST 'cat /data/UserData/move-anything/disabled_modules.json 2>/dev/null || echo "[]"'
echo

echo "4. Manual test required:"
echo "   a) Open Move Anything standalone (Shift+Vol+Knob8)"
echo "   b) Load 'Crash Test' module"
echo "   c) Watch it auto-unload after 3 crashes"
echo "   d) Press ENTER when done..."
read

# Check disabled list after crash
echo "5. Checking disabled modules list (should contain crashtest)..."
DISABLED=$(ssh ableton@$HOST 'cat /data/UserData/move-anything/disabled_modules.json 2>/dev/null || echo "[]"')
echo "   $DISABLED"
if echo "$DISABLED" | grep -q "crashtest"; then
    echo "   ✓ crashtest was auto-disabled"
else
    echo "   ✗ crashtest NOT in disabled list - test failed?"
fi
echo

echo "6. Manual test - Clear Disabled:"
echo "   a) Shift+Vol+Menu → Master FX"
echo "   b) Navigate right to 'Settings'"
echo "   c) Click to enter settings menu"
echo "   d) Find '[Clear 1 Disabled]' and click it"
echo "   e) Press ENTER when done..."
read

# Verify cleared
echo "7. Checking disabled modules list (should be empty again)..."
ssh ableton@$HOST 'cat /data/UserData/move-anything/disabled_modules.json 2>/dev/null || echo "[]"'
echo

echo "8. Manual test - Safe Mode:"
echo "   a) Shift+Vol+Menu → Master FX → Settings"
echo "   b) Find '[Safe Mode]' and click it"
echo "   c) Reboot Move (power cycle or: ssh root@$HOST reboot)"
echo "   d) Verify Move boots into STOCK firmware (no Move Anything)"
echo "   e) Press ENTER when verified..."
read

# Check safe mode log
echo "9. Checking safe mode log..."
ssh ableton@$HOST 'cat /data/UserData/move-anything/safe-mode.log 2>/dev/null || echo "(no log yet)"'
echo

echo "10. Reinstalling Move Anything after safe mode test..."
./scripts/install.sh local --skip-modules
echo

# Test SSH-based safe mode
echo "11. Testing SSH-based safe mode trigger..."
ssh ableton@$HOST 'touch /data/UserData/move-anything/safe-mode'
echo "    Created safe-mode trigger file"
echo "    Rebooting Move..."
ssh root@$HOST reboot || true
echo
echo "    Wait for Move to boot into stock firmware..."
echo "    Press ENTER when verified..."
read

echo "12. Final reinstall..."
./scripts/install.sh local --skip-modules
echo

# Cleanup
echo "13. Cleanup..."
ssh ableton@$HOST 'rm -rf /data/UserData/move-anything/modules/crashtest'
ssh ableton@$HOST 'echo "[]" > /data/UserData/move-anything/disabled_modules.json'
ssh ableton@$HOST 'rm -f /data/UserData/move-anything/safe-mode.log'
echo "   Cleaned up test artifacts"
echo

echo "=== All tests complete ==="
```

## Manual Test Steps

### Test 1: Module Crash Handling

1. Create a crashing module (see script above)
2. Open standalone menu: **Shift+Vol+Knob8**
3. Load "Crash Test" module
4. **Expected:** Module auto-unloads after 3 consecutive `tick()` failures
5. **Verify:** `cat /data/UserData/move-anything/disabled_modules.json` shows `["crashtest"]`

### Test 2: Clear Disabled Modules (Shadow UI)

1. **Shift+Vol+Menu** → Master FX
2. Navigate right to **Settings** component
3. Click to enter settings menu
4. Scroll to **[Clear N Disabled]** and click
5. **Expected:** All disabled modules re-enabled
6. **Verify:** Disabled list is now `[]`

### Test 3: Safe Mode via UI

1. **Shift+Vol+Menu** → Master FX → Settings
2. Scroll to **[Safe Mode]** and click
3. Reboot Move (power cycle or `ssh root@move.local reboot`)
4. **Expected:** Move boots into stock firmware (no Move Anything)
5. **Verify:** `cat /data/UserData/move-anything/safe-mode.log` shows trigger timestamp

### Test 4: Safe Mode via SSH (Emergency Recovery)

```bash
# Create trigger file
ssh ableton@move.local 'touch /data/UserData/move-anything/safe-mode'

# Reboot
ssh root@move.local reboot
```

**Expected:** Move boots into stock firmware. Trigger file is auto-removed.

## Cleanup

```bash
ssh ableton@move.local 'rm -rf /data/UserData/move-anything/modules/crashtest'
ssh ableton@move.local 'echo "[]" > /data/UserData/move-anything/disabled_modules.json'
ssh ableton@move.local 'rm -f /data/UserData/move-anything/safe-mode.log'
```

## Success Criteria

- [ ] Crashing module auto-unloads after 3 failures
- [ ] Crashed module is added to disabled list
- [ ] Disabled modules are skipped during module scan
- [ ] Clear Disabled button re-enables all modules
- [ ] Safe Mode (UI) triggers stock boot on next reboot
- [ ] Safe Mode (SSH) triggers stock boot on next reboot
- [ ] Safe mode trigger file is one-shot (auto-removed)
