# AbleM8

A dual-mode overtake module for Ableton Move that bridges two worlds: full **Dirtywave M8** tracker control via the Launchpad Pro protocol, and **Ableton Live** mixer / device control via the YURS remote script — on the same hardware, switchable in real time.

## Overview

AbleM8 takes over the Move's full UI in shadow mode. It maps Move's pads, knobs, and buttons to the M8 tracker's Launchpad Pro layout, and with a single shortcut, flips the same hardware into an Ableton Live control surface — mute, solo, arm, volume, sends, and device macros.

## Features

**M8 Mode**
- Full Launchpad Pro protocol bridge to the Dirtywave M8 (connected via Move's USB-A port)
- Top and bottom octave views, toggled by jog wheel touch
- Poly aftertouch forwarded as modwheel (channel 3)
- Knobs 1–9 send M8 track parameters, with per-track and global routing
- M8 FX and Master parameter banks accessible (extend via config)
- MIDI clock and sysex pass-through; M8 re-init on device reconnect

**Ableton Live Mode (YURS)**
- Tracks 1–8: mute (row 1), solo (row 2), arm (row 3)
- Knobs 1–8: track volume; hold Shift for Send A
- Master knob: master volume; hold Shift for return volume
- Clip warning: mute LED turns amber when a track volume exceeds threshold
- Shift+Capture: toggle device macro mode (knobs 1–8 → macro CCs 1–8)
- LED feedback driven by YURS note/CC messages from Live
- Bank indicator step LED: green = M8 bank, blue = Ableton bank

## Requirements

- [Move Everything](https://github.com/charlesvestal/move-anything) host installed on your Ableton Move
- Dirtywave M8 (for M8 mode) connected to Move's USB-A port
- [YURS remote script](https://forum.yaeltex.com/t/yurs-yaeltex-universal-remote-script-for-ableton-live/161) installed in Ableton Live (for Ableton Live mode)
  - Configure YURS input/output to the Move's USB MIDI port
  - YURS MIDI channel: **16** (channel index 15)

## Controls

### Universal

| Control | Action |
|---------|--------|
| **Shift + Menu** | Toggle M8 bank ↔ Ableton bank |
| **Shift + Vol + Jog Click** | Exit AbleM8, return to Move |

### M8 Mode

| Control | Action |
|---------|--------|
| **Pads** | LPP pad grid (mapped to M8 display layout) |
| **Jog touch** | Toggle top / bottom octave view |
| **Jog wheel click** | Suppress top/bottom toggle on release |
| **Back / Menu / Capture** | M8 view buttons (Song, Chain, Instrument, etc.) |
| **Play, Rec, Loop, Mute, Undo, Sample** | Forwarded to M8 as LPP buttons |
| **Shift** | Forwarded to M8 |
| **Step buttons 1–8** | Select active track for knob routing |
| **Knobs 1–9** | M8 track parameters (relative encoding) |

### Ableton Live Mode

| Control | Action |
|---------|--------|
| **Pad row 1** | Mute / unmute tracks 1–8 |
| **Pad row 2** | Solo tracks 1–8 |
| **Pad row 3** | Arm tracks 1–8 |
| **Knobs 1–8** | Track volume (or Send A while Shift held) |
| **Master knob** | Master volume (or return volume while Shift held) |
| **Mute button** | Panic — set master volume to 0 |
| **Shift + Capture** | Toggle device macro mode |
| **Knobs 1–8 (macro mode)** | Device macro CCs 1–8 |

## Setup

### M8 Mode

1. Connect your M8 to the Move's USB-A port.
2. Enter shadow mode: **Shift + Vol + Knob 1**
3. Navigate to **Overtake Modules → AbleM8**
4. AbleM8 will send the LPP handshake to the M8 automatically on init.
   If the M8 doesn't respond, it will retry when it sends its own SysEx identity request.

### Ableton Live Mode

1. Install the YURS remote script in your Live library and add it as a control surface:
   - **Control Surface**: YURS
   - **Input / Output**: Move's USB MIDI port
2. In your Live set, configure YURS for 8 tracks on channel 16.
3. Switch to Ableton bank with **Shift + Menu** while in AbleM8.
4. LED feedback (mute/solo/arm state, volume levels) flows from Live automatically.

### YURS CC/Note Map (default `ablem8_config.mjs`)

| Function | Notes / CCs |
|----------|------------|
| Mute (notes) | 0 – 7 |
| Solo (notes) | 16 – 23 |
| Arm (notes) | 32 – 39 |
| Volume (CCs) | 0 – 7 |
| Send A (CCs) | 16 – 23 |
| Macros (CCs) | 32 – 39 |
| Master volume (CC) | 127 |
| Return volume (CC) | 117 |

Edit `config/ablem8_config.mjs` to match your YURS configuration.

## Configuration

Open [config/ablem8_config.mjs](config/ablem8_config.mjs) to adjust:

- `midiChannel` — YURS MIDI channel (default 16 / index 15)
- `encoderMode` — `"relative"` (default) or `"absolute"`
- `notes` / `ccs` — remap YURS note and CC assignments
- `clipWarningThreshold` — volume level (0–127) at which mute LED turns amber (default 118)

## Acknowledgements

AbleM8 was built on the work of many people in the Move, M8, and Ableton communities.

### move-anything

The foundation for all Move Everything modules. Thanks to [bobbydigitales](https://github.com/bobbydigitales/move-anything) and every contributor who mapped out the Move's hardware interface and built the open ecosystem this module runs on.

Special thanks to **Tim Lamb** for early LPP protocol research and community contributions that informed the M8 ↔ Move mapping work.

### Dirtywave M8

The M8 is a beautiful piece of hardware. Dirtywave publishes resources for developers and the community:
[dirtywave.com/pages/resources-downloads](https://dirtywave.com/pages/resources-downloads)

### M8 + LPP Protocol Reference

The M8 Launchpad Pro control protocol is community-documented. This excellent reference by [grahack](https://github.com/grahack) was essential for mapping the LPP note grid and control layout to Move's hardware:
[grahack.github.io/M8_LPP_recap](https://grahack.github.io/M8_LPP_recap/)

### Ableton Move

Hardware and MIDI specifications from the official Ableton Move manual:
[ableton.com/en/move/manual](https://www.ableton.com/en/move/manual/)

### YURS Remote Script

The Ableton Live integration uses the YURS (Yaeltex Universal Remote Script) protocol.
[YURS on the Yaeltex forum](https://forum.yaeltex.com/t/yurs-yaeltex-universal-remote-script-for-ableton-live/161)

### LPP3 Programming Reference

Focusrite's Launchpad Pro Mk3 programming reference guided the SysEx handshake and LED color encoding:
[LPP3 Programmer Reference Guide](https://fael-downloads-prod.focusrite.com/customer/prod/s3fs-public/downloads/LPP3_prog_ref_guide_200415.pdf)

## AI Assistance Disclaimer

This module is part of Move Everything and was developed with AI assistance, including Claude and other AI tools.

All architecture, implementation, and release decisions are reviewed by human maintainers.
AI-assisted content may still contain errors — please validate functionality, security, and license compatibility before production use.
