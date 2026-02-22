#!/usr/bin/env bash
set -euo pipefail

# Verify shim supports experimental native-Move chord expansion:
# - Pre-ioctl: captures MIDI_OUT notes (before SPI destroys them)
# - Post-ioctl: injects chord extras into MIDI_IN as cable-2 external MIDI
# This checks for:
# - dedicated transform helper
# - lookup of slot MIDI FX module/type
# - pre-ioctl generation function
# - post-ioctl injection of pre-generated extras
# - explicit cable-2 reinjection for transformed packets.

file="src/move_anything_shim.c"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

if ! rg -n "shadow_transform_musical_note_for_move" "$file" >/dev/null 2>&1; then
  echo "FAIL: Missing musical-note chord transform helper" >&2
  exit 1
fi

if ! rg -n "midi_fx1_module" "$file" >/dev/null 2>&1; then
  echo "FAIL: Missing midi_fx1_module lookup for transform" >&2
  exit 1
fi

if ! rg -n "midi_fx1:type" "$file" >/dev/null 2>&1; then
  echo "FAIL: Missing midi_fx1:type lookup for transform" >&2
  exit 1
fi

if ! rg -n "shadow_generate_chord_extras_pre_ioctl" "$file" >/dev/null 2>&1; then
  echo "FAIL: Missing pre-ioctl chord generation function" >&2
  exit 1
fi

if ! rg -n "shadow_pre_chord_extras" "$file" >/dev/null 2>&1; then
  echo "FAIL: Missing pre-ioctl chord extras buffer" >&2
  exit 1
fi

if ! rg -n "0x02 << 4" "$file" >/dev/null 2>&1; then
  echo "FAIL: Missing cable-2 reinjection format for transformed packets" >&2
  exit 1
fi

echo "PASS: Native Move musical-note chord transform hook is present"
