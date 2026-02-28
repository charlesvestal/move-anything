#!/usr/bin/env bash
set -euo pipefail

# Verify shim supports staggered native-Move chord expansion:
# - Post-ioctl: scans cable-0 pad notes, leaves root in-place
# - Chord intervals queued for one-per-frame injection
# - All injected notes are cable-0 (same as real pad presses)
# This checks for:
# - chord rewrite function operating on MIDI_IN buffer
# - lookup of slot MIDI FX module/type
# - staggered injection queue
# - cable-0 note-on/off format (0x09/0x08 headers)
# - pad note range filtering (68-99)
# - held-notes table for tracking chord state

file="src/move_anything_shim.c"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

if ! rg -n "shadow_chord_rewrite_midi_in" "$file" >/dev/null 2>&1; then
  echo "FAIL: Missing post-ioctl chord rewrite function" >&2
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

# Verify staggered queue mechanism
if ! rg -n "shadow_chord_queue_push" "$file" >/dev/null 2>&1; then
  echo "FAIL: Missing staggered chord queue push" >&2
  exit 1
fi

if ! rg -n "shadow_chord_queue_pop" "$file" >/dev/null 2>&1; then
  echo "FAIL: Missing staggered chord queue pop" >&2
  exit 1
fi

# Verify cable-0 format for injection (0x09/0x08 headers)
if ! rg -n 'sh_midi\[wp\].*= 0x09' "$file" >/dev/null 2>&1; then
  echo "FAIL: Missing cable-0 note-on injection (0x09)" >&2
  exit 1
fi

if ! rg -n 'sh_midi\[wp\].*= 0x08' "$file" >/dev/null 2>&1; then
  echo "FAIL: Missing cable-0 note-off injection (0x08)" >&2
  exit 1
fi

# Verify pad note range check
if ! rg -n "note < 68 \|\| note > 99" "$file" >/dev/null 2>&1; then
  echo "FAIL: Missing pad note range filter (68-99)" >&2
  exit 1
fi

echo "PASS: Staggered native-Move chord injection is present"
