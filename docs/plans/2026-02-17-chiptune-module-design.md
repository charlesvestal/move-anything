# Chiptune Module Design

## Overview

A sound generator module emulating both the NES 2A03 and Game Boy DMG APUs for authentic chiptune synthesis on Move hardware. Paraphonic voice allocation, factory presets, and full Signal Chain integration.

## Chip Emulation

Two APU engines in a single module, switchable per-preset:

- **NES (2A03)** via blargg's Nes_Snd_Emu (LGPL-2.1)
  - 2 pulse channels (12.5/25/50/75% duty, hardware sweep)
  - 1 triangle channel (fixed timbre)
  - 1 noise channel (short/long LFSR)
  - 1 DPCM channel (sample playback, future use)

- **Game Boy (DMG)** via gb_apu by ITotalJustice (MIT)
  - 2 square channels (12.5/25/50/75% duty, ch1 has hardware sweep)
  - 1 wave channel (32-sample 4-bit wavetable)
  - 1 noise channel (7-bit/15-bit LFSR)

Both libraries handle resampling to 44100Hz via bandlimited synthesis (Blip_Buffer).

## Voice Allocation

Paraphonic allocator assigns MIDI notes to available hardware channels.

**Modes:**
- **Auto**: All melodic channels available. Noise reserved for high notes (96+) or special velocity.
- **Lead**: Monophonic, one channel, portamento support.
- **Locked**: Preset assigns channel types to MIDI note ranges (e.g., pulse=melody, triangle=bass, noise=percussion).

When all channels are used, oldest note is stolen.

## MIDI Mapping

| Knob | Parameter | Description |
|------|-----------|-------------|
| 1 | Duty / Wave shape | Pulse duty cycle or wavetable selection |
| 2 | Envelope Attack | Volume envelope attack time |
| 3 | Envelope Decay | Volume envelope decay/release |
| 4 | Sweep | Pitch sweep amount |
| 5 | Vibrato depth | LFO pitch modulation depth |
| 6 | Vibrato rate | LFO speed |
| 7 | Noise tone | Noise mode and pitch |
| 8 | Volume | Output level |

## Factory Presets (~22)

```
NES Presets:
  0  NES Square Lead     - Pulse 50% duty, moderate decay
  1  NES Bright Lead     - Pulse 25% duty, short decay
  2  NES Thin Lead       - Pulse 12.5% duty, plucky
  3  NES Duo Lead        - Both pulses detuned slightly
  4  NES Triangle Bass   - Triangle channel, deep bass
  5  NES Tri Sub         - Triangle, octave -1
  6  NES Chip Arp        - Fast auto-arp across pulse channels
  7  NES Noise Hat       - Short noise, high pitch
  8  NES Noise Snare     - Long noise, medium pitch
  9  NES Noise Kick      - Noise + triangle pitch sweep down
 10  NES Full Kit        - Locked mode: pulse melody + tri bass + noise perc
 11  NES Power Chord     - Pulse 1+2 at fifth interval

GB Presets:
 12  GB Square Lead      - Square 50%, sweep off
 13  GB Sweep Lead       - Square 1 with pitch sweep
 14  GB Pulse Duo        - Both squares, slight detune
 15  GB Wave Bass        - Wavetable with bass waveform
 16  GB Wave Pad         - Wavetable with soft waveform, slow attack
 17  GB Wave Growl       - Wavetable with harsh waveform
 18  GB Noise Hat        - Short noise, 7-bit LFSR
 19  GB Noise Snare      - Long noise
 20  GB Full Kit         - Locked mode across all 4 channels
 21  GB Chiptune Classic - Sq1 lead + Sq2 harmony + Wave bass + Noise perc
```

## Preset Data Structure

```c
typedef struct {
    char name[32];
    uint8_t chip;           // CHIP_NES or CHIP_GB
    uint8_t alloc_mode;     // AUTO, LEAD, LOCKED
    uint8_t duty;           // pulse/square duty cycle (0-3)
    uint8_t env_attack;
    uint8_t env_decay;
    int8_t  sweep;          // pitch sweep (-7..+7, 0=off)
    uint8_t vibrato_depth;
    uint8_t vibrato_rate;
    uint8_t noise_mode;     // short/long LFSR
    uint8_t noise_pitch;
    uint8_t wavetable_idx;  // GB wave channel waveform selection
    uint8_t channel_mask;   // which channels enabled (bitmask)
    int8_t  detune;         // for duo/chord presets
    int8_t  octave_offset;  // per-channel octave shift for locked mode
    uint8_t volume;
} chiptune_preset_t;
```

## UI Hierarchy (Shadow UI)

```
Root: Preset browser (scroll presets by name)
  └── Main: Knob parameters + menu items
        ├── Chip (NES/GB)
        ├── Duty, Attack, Decay, Sweep, Vibrato, Noise, Wavetable, Volume
        ├── Voice Mode (Auto/Lead/Locked)
        └── Channel Config (per-channel enable/disable)
```

## DSP Plugin

**API:** Plugin API v2 (required for Signal Chain, multiple instances)

**render_block flow (128 frames @ 44100Hz):**
1. Advance software envelopes for active voices
2. Advance LFO, compute vibrato offset
3. Per active voice: apply vibrato to frequency, apply envelope to volume, write APU registers
4. Clock APU emulation for 128 frames of cycles
5. Read mono samples from APU, duplicate to stereo int16 output

**Software envelopes:** The hardware envelopes on both chips are limited (NES has 4-bit linear decay, GB has 4-bit linear up/down). Software ADSR envelopes modulate APU volume registers per render block for smoother, more musical control.

## Repository Structure

```
move-anything-chiptune/
  src/
    module.json
    ui.js
    dsp/
      chiptune_plugin.cpp      # Plugin API v2, voice allocator, presets
      chiptune_presets.h        # Factory preset definitions
      midi_to_apu.h            # MIDI note to APU period tables
      voice_alloc.h            # Paraphonic voice allocator
    libs/
      nes_snd_emu/             # Git submodule (LGPL-2.1)
      gb_apu/                  # Git submodule (MIT)
  patches/
    Chiptune NES.json          # Signal Chain preset
    Chiptune GB.json           # Signal Chain preset
  scripts/
    build.sh
    install.sh
    Dockerfile
  .github/workflows/release.yml
```

## Dependencies

| Library | License | Source |
|---------|---------|--------|
| Nes_Snd_Emu (jamesathey fork) | LGPL-2.1 | github.com/jamesathey/Nes_Snd_Emu |
| gb_apu (ITotalJustice) | MIT | github.com/ITotalJustice/gb_apu |

LGPL compliance: NES APU statically linked into dsp.so, which is dynamically loaded by chain host via dlopen. Users can relink.

## Signal Chain Integration

- `component_type: "sound_generator"` — chainable
- `chain_params` expose all tweakable parameters with proper types/ranges
- Standard preset protocol: `preset`, `preset_count`, `preset_name` via get_param
- Installs to `modules/sound_generators/chiptune/`
- Ships with 2 starter chain patches
