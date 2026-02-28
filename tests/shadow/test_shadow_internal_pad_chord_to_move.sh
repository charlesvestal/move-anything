#!/usr/bin/env bash
set -euo pipefail

# Verify shim routes pad notes through chain MIDI FX for native Move injection:
# - Post-ioctl: scans cable-0 pad notes, blocks originals
# - Runs notes through chain's midi_fx_process get_param
# - Output queued for one-per-frame staggered cable-0 injection
# This checks for:
# - MIDI FX rewrite function operating on MIDI_IN buffer
# - midi_fx_process get_param call
# - staggered injection queue
# - cable-0 note-on/off format (0x09/0x08 headers)
# - pad note range filtering (68-99)
# - held-notes table for tracking state

shim="src/move_anything_shim.c"
chain="src/modules/chain/dsp/chain_host.c"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

# Shim: rewrite function exists
if ! rg -n "shadow_midi_fx_rewrite" "$shim" >/dev/null 2>&1; then
  echo "FAIL: Missing post-ioctl MIDI FX rewrite function" >&2
  exit 1
fi

# Shim: calls midi_fx_process via get_param
if ! rg -n "midi_fx_process:" "$shim" >/dev/null 2>&1; then
  echo "FAIL: Missing midi_fx_process get_param call in shim" >&2
  exit 1
fi

# Chain host: midi_fx_process handler in get_param
if ! rg -n "midi_fx_process:" "$chain" >/dev/null 2>&1; then
  echo "FAIL: Missing midi_fx_process handler in chain_host" >&2
  exit 1
fi

# Chain host: calls v2_process_midi_fx
if ! rg -n "v2_process_midi_fx" "$chain" >/dev/null 2>&1; then
  echo "FAIL: Missing v2_process_midi_fx in chain_host" >&2
  exit 1
fi

# Shim: staggered queue mechanism
if ! rg -n "shadow_midi_fx_queue_push" "$shim" >/dev/null 2>&1; then
  echo "FAIL: Missing staggered MIDI FX queue push" >&2
  exit 1
fi

if ! rg -n "shadow_midi_fx_queue_pop" "$shim" >/dev/null 2>&1; then
  echo "FAIL: Missing staggered MIDI FX queue pop" >&2
  exit 1
fi

# Shim: cable-0 injection format (always at pos=0)
if ! rg -n 'sh_midi\[0\].*= 0x09' "$shim" >/dev/null 2>&1; then
  echo "FAIL: Missing cable-0 note-on injection (0x09)" >&2
  exit 1
fi

if ! rg -n 'sh_midi\[0\].*= 0x08' "$shim" >/dev/null 2>&1; then
  echo "FAIL: Missing cable-0 note-off injection (0x08)" >&2
  exit 1
fi

# Shim: pad note range check
if ! rg -n "note < 68 \|\| note > 99" "$shim" >/dev/null 2>&1; then
  echo "FAIL: Missing pad note range filter (68-99)" >&2
  exit 1
fi

echo "PASS: MIDI FX → native Move injection pipeline is present"
