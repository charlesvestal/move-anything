# Native Move MIDI FX Findings

This note documents the current branch findings for the experimental path that
tries to make Shadow-chain MIDI FX drive native Move synths by injecting
transformed notes into Move's mailbox `MIDI_IN`.

## Summary

The native Move synth path is not stable enough for live transformed-note
injection via mailbox `MIDI_IN`.

What does work:
- Shadow DSP / Shadow-chain MIDI routing
- Overtake modules talking to Shadow DSP slots
- Single-root chord injection some of the time

What does not appear reliable:
- Generic MIDI FX for native Move synths via mailbox `MIDI_IN`
- Repeated or overlapping transformed note injection into native Move

The current evidence points to Move aborting on internal event-buffer ordering
invariants, not to a simple bug in held-note bookkeeping.

## Goal

The goal was to reuse Shadow-chain MIDI FX, especially `chord`, so that a Move
track could send MIDI into a native Move synth and receive transformed notes
before the native synth voice engine.

The desired user-facing behavior was:
- play one note on a Move track
- Shadow `chord` expands it
- native Move synth receives the expanded notes
- no crash, no runaway note state, acceptable latency

## Existing MIDI Paths

There are two different MIDI paths in this codebase:

1. Shadow DSP path

- Used by Overtake and other Shadow-side DSP routing
- Feeds MIDI to Shadow DSP slots
- Does not directly drive native Move synth voices

2. Native Move path

- Captures cable-2 musical events from `MIDI_OUT` before the SPI transaction
  destroys them
- Computes transformed notes in the shim
- Injects synthetic events into the Shadow mailbox `MIDI_IN` after `ioctl()`
- Intended to reach native Move's own synth/event pipeline

This distinction matters: techniques that are safe for Shadow DSP routing are
not automatically safe for native Move mailbox injection.

## Implementation Experiments

The branch tried the following approaches in
[`src/move_anything_shim.c`](../src/move_anything_shim.c):

1. FIFO-style pending chord event queue

- Worked intermittently
- Produced audible delay
- Could cascade or self-chord when suppression/note-off timing was wrong

2. Suppression lifecycle fixes

- Delayed suppression release until actual note-off injection
- Prevented some self-feedback loops

3. Pending note cancellation and per-note coalescing

- Replaced the old queue-heavy model with per-channel/per-note desired state
- Removed most audible backlog and cascade behavior

4. Injection pacing and diagnostics

- Added short cooldowns between synthetic transitions
- Added state summaries, stall logs, and abort/assert hooks
- Added backtrace capture in the shim's `abort()` hook

5. Busy-cycle gating

- Attempted to avoid injecting into mailbox frames that were already busy
- Overly strict gating starved chord injection entirely
- Narrowing the gate restored chords but did not remove the crash

6. Monophonic transformed-root guard

- Prevented overlapping transformed roots on the same channel
- Confirmed overlap was one trigger
- Did not eliminate the abort

## Device Observations

Observed behavior during on-device testing:

- Initial versions could cascade chord notes and sometimes hang
- Queue/state fixes removed most delay and self-chording
- Repeated single-note playing could still abort Move
- Overlapping held roots often triggered the abort faster
- After overlap suppression was added, the abort still occurred on a later
  single-root chord after state had fully returned to zero

This last point is important: overlap is not the only issue.

In the latest repro:

- overlapping transformed roots were explicitly skipped
- held/desired/injected state returned to zero
- a fresh single-root chord (`64 -> 68,71`) was injected
- Move aborted immediately after the injected note-on

That rules out "only overlapping roots" as the root cause.

## Crash Evidence

The shim now logs plain `abort()` calls and backtraces before termination.

The repeated device-side backtrace stays consistent:

```text
/opt/move/MoveOriginal(+0x1137498)
/opt/move/MoveOriginal(+0x1129d4c)
/opt/move/MoveOriginal(+0x112952c)
/opt/move/MoveOriginal(+0xf80ce8)
/opt/move/MoveOriginal(+0xf7ffa4)
/opt/move/MoveOriginal(+0xf81374)
```

To understand those abort sites, `MoveOriginal` was copied locally and the
rodata strings behind the abort/assert paths were extracted. The relevant
strings are:

- `mBuffer.size() > 0`
- `!mBuffer.empty() && mBuffer[0].time == Time{0.0}`
- `!maybeNewTime || maybeNewTime > moTime`
- `../../../../shared/engine-core/EngineCoreLib/include/ableton/engine/CvBuffer.hpp`
- `../../../../shared/engine-core/EngineCoreLib/include/ableton/engine/detail/ProcessEventsStepper.hpp`

Interpretation:

- Move is aborting on internal engine-core event-buffer invariants
- the failure is about event ordering/timing assumptions inside Move
- this does not look like random memory corruption from the shim

## Findings

1. Overtake/Shadow DSP methods do not transfer directly to native Move synths.

2. Mailbox `MIDI_IN` injection can be made "less bad" in terms of delay and
   feedback, but it still violates native Move's internal event assumptions.

3. The held-note/suppression bookkeeping in the shim can be coherent while Move
   still aborts internally.

4. Overlap makes the problem easier to reproduce, but overlap suppression alone
   does not fix it.

5. This path should not be generalized to `arp` or other MIDI FX in its current
   form.

## Approach 2: Subtractive Rewrite (Current)

After the additive approach failed, a subtractive approach was implemented:

Instead of **adding** extra cable-2 notes alongside the original pad note,
the shim now **rewrites** MIDI_IN in the post-ioctl copy. When a chord is
active:

1. The hardware MIDI_IN buffer is copied to the shadow mailbox as usual
2. `shadow_chord_rewrite_midi_in()` scans for cable-0 pad notes (68-99)
3. The original note-on stays in place (root of chord)
4. Chord interval notes are written into subsequent empty buffer slots
5. All injected notes use cable-0 format (`0x09`/`0x08` headers)

Key differences from the additive approach:

- **No cable-2**: All events are cable-0, identical to real pad presses
- **No echo suppression needed**: No feedback loop since we don't inject
  on a separate cable
- **Same-frame delivery**: Root and intervals are in the same buffer frame
- **No rate limiting**: All chord notes appear atomically in one frame

The hypothesis is that Move already handles multiple cable-0 note events
per frame (polyphonic pad playing), so chord-expanded notes in the same
format should not violate `ProcessEventsStepper` ordering invariants.

### Risks

- Move may have per-pad state that expects specific note numbers from the
  hardware scan. Synthetic notes without corresponding pad touches could
  confuse pad-state tracking.
- Pad LED feedback may desync since the hardware only registers one pad
  press but the synth sees multiple notes.
- If Move's engine processes cable-0 notes differently from how they appear
  in the USB-MIDI buffer (e.g., with per-pad timestamps from the scanner),
  the synthetic notes may still trigger ordering violations.

## Branch Artifacts

This branch contains useful debugging artifacts:

- `move-chord` state logging (flag-file gated)
- abort/assert diagnostics and backtrace capture
- MIDI injection test harness (flag-file triggered cable/channel combos)
- richer mailbox probe and ALSA seq trace tools
