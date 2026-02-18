# Chiptune Module Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Build a Move Anything sound generator module emulating NES 2A03 and Game Boy DMG APUs with paraphonic voice allocation, factory presets, and Signal Chain integration.

**Architecture:** Plugin API v2 module with two APU backends (Nes_Snd_Emu for NES, gb_apu for Game Boy) switchable per-preset. A voice allocator maps MIDI notes to hardware channels. Software envelopes and LFO supplement the limited hardware envelopes.

**Tech Stack:** C++ (plugin wrapper), C (gb_apu), Nes_Snd_Emu (C++ LGPL-2.1), gb_apu (C MIT), Docker cross-compilation for ARM64.

**Reference module:** `move-anything-braids` — copy its patterns for build.sh, Dockerfile, install.sh, release.yml, param_helper.h, and plugin API v2 lifecycle.

---

### Task 1: Create Repository and Build Scaffolding

**Files:**
- Create: `move-anything-chiptune/scripts/Dockerfile`
- Create: `move-anything-chiptune/scripts/build.sh`
- Create: `move-anything-chiptune/scripts/install.sh`
- Create: `move-anything-chiptune/.github/workflows/release.yml`
- Create: `move-anything-chiptune/.gitignore`
- Create: `move-anything-chiptune/src/module.json`
- Create: `move-anything-chiptune/release.json`

**Step 1: Initialize the repository**

```bash
cd /Volumes/ExtFS/charlesvestal/github/move-everything-parent
mkdir move-anything-chiptune
cd move-anything-chiptune
git init
```

**Step 2: Add APU library submodules**

```bash
cd /Volumes/ExtFS/charlesvestal/github/move-everything-parent/move-anything-chiptune
git submodule add https://github.com/jamesathey/Nes_Snd_Emu.git src/libs/nes_snd_emu
git submodule add https://github.com/ITotalJustice/gb_apu.git src/libs/gb_apu
```

**Step 3: Create .gitignore**

```
build/
dist/
*.o
*.so
.DS_Store
```

**Step 4: Create Dockerfile** (copy from braids)

```dockerfile
# Chiptune Module Build Environment
# Targets: Ableton Move (aarch64 Linux)

FROM debian:bookworm

RUN apt-get update && apt-get install -y \
    gcc-aarch64-linux-gnu \
    g++-aarch64-linux-gnu \
    make \
    file \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build

# Set cross-compilation environment
ENV CROSS_PREFIX=aarch64-linux-gnu-
ENV CC=aarch64-linux-gnu-gcc
ENV CXX=aarch64-linux-gnu-g++
```

**Step 5: Create module.json**

```json
{
  "id": "chiptune",
  "name": "Chiptune",
  "abbrev": "CT",
  "version": "0.1.0",
  "description": "NES & Game Boy chiptune synthesizer",
  "author": "charlesvestal",
  "license": "MIT/LGPL-2.1",
  "dsp": "dsp.so",
  "api_version": 2,
  "standalone": false,
  "capabilities": {
    "audio_out": true,
    "audio_in": false,
    "midi_in": true,
    "midi_out": false,
    "aftertouch": false,
    "chainable": true,
    "component_type": "sound_generator"
  }
}
```

**Step 6: Create release.json**

```json
{
  "version": "0.1.0",
  "download_url": ""
}
```

**Step 7: Create build.sh** (stub that compiles a minimal plugin)

Create `scripts/build.sh` following the braids pattern exactly:
- Docker wrapper if `CROSS_PREFIX` not set
- Cross-compile C and C++ sources
- Link into `dsp.so`
- Package to `dist/chiptune/` with module.json
- Create `dist/chiptune-module.tar.gz`

The initial build.sh will be fleshed out in Task 2 once we have actual source files.

**Step 8: Create install.sh** (copy braids pattern, change `braids` → `chiptune`)

**Step 9: Create release.yml** (copy braids pattern, change `braids` → `chiptune`)

**Step 10: Commit**

```bash
git add -A
git commit -m "chore: initial repository scaffolding"
```

---

### Task 2: Minimal Plugin Skeleton (No Sound)

**Files:**
- Create: `move-anything-chiptune/src/dsp/chiptune_plugin.cpp`
- Create: `move-anything-chiptune/src/dsp/param_helper.h` (copy from braids)
- Modify: `move-anything-chiptune/scripts/build.sh` (add actual compilation)

**Goal:** A plugin that loads in Signal Chain, accepts MIDI, outputs silence. Proves the build pipeline and plugin API v2 integration work.

**Step 1: Copy param_helper.h from braids**

Copy `/Volumes/ExtFS/charlesvestal/github/move-everything-parent/move-anything-braids/src/dsp/param_helper.h` to `src/dsp/param_helper.h`.

**Step 2: Create chiptune_plugin.cpp with minimal plugin API v2**

The skeleton must include:
- Include guards and headers: `<stdint.h>`, `<string.h>`, `<stdlib.h>`, `<stdio.h>`, `"param_helper.h"`
- Forward-declare `host_api_v1_t` and `plugin_api_v2_t` structs (or include the header — but since we don't want a dependency on the main repo, inline the struct definitions as braids does)
- Define `MOVE_PLUGIN_API_VERSION_2 2`
- Instance struct `chiptune_instance_t` with just:
  - `char module_dir[256]`
  - `int current_preset`, `int preset_count`, `char preset_name[64]`
- Implement all 7 v2 functions:
  - `v2_create_instance`: calloc instance, set preset_count=1, preset_name="Init"
  - `v2_destroy_instance`: free
  - `v2_on_midi`: parse status byte, log "note on/off" only
  - `v2_set_param`: handle "preset", "state"
  - `v2_get_param`: handle "name", "preset", "preset_count", "preset_name", "ui_hierarchy", "chain_params", "state"
  - `v2_get_error`: return 0
  - `v2_render_block`: `memset(out, 0, frames * 4)`
- `extern "C" move_plugin_init_v2` entry point

For `ui_hierarchy`, return a minimal JSON:
```json
{"modes":null,"levels":{"root":{"list_param":"preset","count_param":"preset_count","name_param":"preset_name","children":null,"knobs":[],"params":[]}}}
```

For `chain_params`, return `[]`.

For `state`, return `{"preset":0}`.

**Step 3: Update build.sh with actual compilation**

```bash
# Compile plugin (no APU libs yet)
${CROSS_PREFIX}g++ -g -O3 -shared -fPIC -std=c++14 \
    src/dsp/chiptune_plugin.cpp \
    -o build/dsp.so \
    -Isrc/dsp \
    -lm
```

**Step 4: Build and verify**

```bash
cd /Volumes/ExtFS/charlesvestal/github/move-everything-parent/move-anything-chiptune
./scripts/build.sh
file dist/chiptune/dsp.so  # Should show: ELF 64-bit LSB shared object, ARM aarch64
```

**Step 5: Deploy and verify loads**

```bash
./scripts/install.sh
# On device: load chiptune in Signal Chain, verify it appears and loads without crash
```

**Step 6: Commit**

```bash
git add -A
git commit -m "feat: minimal plugin skeleton (loads in Signal Chain, outputs silence)"
```

---

### Task 3: NES APU Integration — Pulse Channel Sound

**Files:**
- Modify: `move-anything-chiptune/src/dsp/chiptune_plugin.cpp`
- Modify: `move-anything-chiptune/scripts/build.sh`

**Goal:** Get a single NES pulse channel producing sound from MIDI note-on. Proves the Nes_Snd_Emu library compiles and outputs audio correctly.

**Step 1: Study Nes_Snd_Emu API**

The library provides `Simple_Apu` (in `nes_apu/Simple_Apu.h`):
- `Simple_Apu apu;`
- `apu.sample_rate(44100);` — set output sample rate
- `apu.write_register(cpu_time, addr, data);` — write APU register
- `apu.end_frame(cpu_time);` — finish frame, generate samples
- `apu.read_samples(buf, count);` — read generated samples into buffer

CPU clock: 1789773 Hz (NTSC). For 128 frames at 44100 Hz:
- `cpu_cycles_per_frame = 1789773 / 44100 * 128 ≈ 5186`

NES Pulse 1 registers:
- `$4000`: duty (bits 7-6), volume (bits 3-0), constant vol (bit 4)
- `$4001`: sweep (disabled: `$00`)
- `$4002`: period low 8 bits
- `$4003`: period high 3 bits (bits 2-0) + length counter load (bits 7-3)

MIDI note to NES period: `period = 1789773 / (16 * freq) - 1` where `freq = 440 * 2^((note-69)/12)`

Enable pulse 1: write `$4015` = `0x01`

**Step 2: Add Nes_Snd_Emu to instance struct**

```cpp
#include "Simple_Apu.h"

typedef struct {
    char module_dir[256];
    Simple_Apu nes_apu;
    // ... existing fields
} chiptune_instance_t;
```

**Step 3: Initialize NES APU in create_instance**

```cpp
inst->nes_apu.sample_rate(44100);
// Enable pulse 1
inst->nes_apu.write_register(0, 0x4015, 0x01);
```

**Step 4: Handle MIDI note-on → write pulse 1 registers**

In `v2_on_midi`, on note-on (0x90, velocity > 0):
```cpp
float freq = 440.0f * powf(2.0f, (note - 69) / 12.0f);
int period = (int)(1789773.0f / (16.0f * freq) - 1);
if (period < 0) period = 0;
if (period > 0x7FF) period = 0x7FF;

// Duty 50%, constant volume, velocity-scaled volume
uint8_t vol = (data2 * 15) / 127;
inst->nes_apu.write_register(0, 0x4000, 0xB0 | vol);  // duty=10, const vol, vol
inst->nes_apu.write_register(0, 0x4001, 0x00);          // sweep off
inst->nes_apu.write_register(0, 0x4002, period & 0xFF); // period low
inst->nes_apu.write_register(0, 0x4003, (period >> 8) & 0x07); // period high
```

On note-off: write volume 0
```cpp
inst->nes_apu.write_register(0, 0x4000, 0xB0);  // volume = 0
```

**Step 5: Implement render_block with NES APU**

```cpp
static void v2_render_block(void *instance, int16_t *out_interleaved_lr, int frames) {
    chiptune_instance_t *inst = (chiptune_instance_t*)instance;
    memset(out_interleaved_lr, 0, frames * 4);
    if (!inst) return;

    // Clock the APU
    long cpu_cycles = (long)(1789773.0 / 44100.0 * frames);
    inst->nes_apu.end_frame(cpu_cycles);

    // Read mono samples
    int16_t mono[128];
    int count = inst->nes_apu.read_samples(mono, frames);

    // Convert mono to stereo interleaved, scale up (APU output is quiet)
    for (int i = 0; i < count; i++) {
        int32_t s = mono[i] * 4;  // Boost
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        out_interleaved_lr[i * 2] = (int16_t)s;
        out_interleaved_lr[i * 2 + 1] = (int16_t)s;
    }
}
```

**Step 6: Update build.sh to compile Nes_Snd_Emu sources**

The Nes_Snd_Emu library files we need to compile (from `src/libs/nes_snd_emu/nes_apu/`):
- `Nes_Apu.cpp`
- `Nes_Oscs.cpp`
- `Blip_Buffer.cpp`
- `Simple_Apu.cpp`
- `apu_snapshot.cpp`

```bash
# Compile NES APU library
NES_SRCS="
    src/libs/nes_snd_emu/nes_apu/Nes_Apu.cpp
    src/libs/nes_snd_emu/nes_apu/Nes_Oscs.cpp
    src/libs/nes_snd_emu/nes_apu/Blip_Buffer.cpp
    src/libs/nes_snd_emu/nes_apu/Simple_Apu.cpp
    src/libs/nes_snd_emu/nes_apu/apu_snapshot.cpp
"
for src in $NES_SRCS; do
    obj="build/$(basename "$src" .cpp).o"
    ${CROSS_PREFIX}g++ -g -O3 -fPIC -std=c++14 \
        -I src/libs/nes_snd_emu \
        -c "$src" -o "$obj"
done

# Compile plugin
${CROSS_PREFIX}g++ -g -O3 -fPIC -std=c++14 \
    -I src/dsp \
    -I src/libs/nes_snd_emu \
    -c src/dsp/chiptune_plugin.cpp \
    -o build/chiptune_plugin.o

# Link
${CROSS_PREFIX}g++ -shared \
    build/chiptune_plugin.o \
    build/Nes_Apu.o build/Nes_Oscs.o build/Blip_Buffer.o \
    build/Simple_Apu.o build/apu_snapshot.o \
    -o build/dsp.so -lm
```

NOTE: The exact source file names may differ — inspect `src/libs/nes_snd_emu/` after submodule checkout to confirm. The library may use different file organization. Adjust accordingly.

**Step 7: Build and verify**

```bash
./scripts/build.sh
```

**Step 8: Deploy and test**

```bash
./scripts/install.sh
# On device: load chiptune, play pads — should hear NES pulse wave
```

**Step 9: Commit**

```bash
git add -A
git commit -m "feat: NES APU integration - pulse channel produces sound from MIDI"
```

---

### Task 4: Game Boy APU Integration — Square Channel Sound

**Files:**
- Modify: `move-anything-chiptune/src/dsp/chiptune_plugin.cpp`
- Modify: `move-anything-chiptune/scripts/build.sh`

**Goal:** Add gb_apu backend. A "chip" parameter switches between NES and GB mode.

**Step 1: Study gb_apu API**

The gb_apu library (C API from `src/libs/gb_apu/include/gb_apu.h`):
- `struct gb_apu apu;`
- `gb_apu_init(&apu, use_cxx_blip);` — initialize
- `gb_apu_set_sample_rate(&apu, 44100);` — set output rate
- `gb_apu_bus_write(&apu, addr, value);` — write register
- `gb_apu_run(&apu, cycles);` — clock emulation
- `gb_apu_samples_avail(&apu);` — check available samples
- `gb_apu_read_samples(&apu, buf, count);` — read samples

GB CPU clock: 4194304 Hz. For 128 frames at 44100 Hz:
- `cpu_cycles_per_frame = 4194304 / 44100 * 128 ≈ 12160`

GB frame sequencer must be clocked: `gb_apu_frame_sequencer_clock(&apu)` on DIV bit 4 falling edge. In practice, call it at 512 Hz (every ~8192 CPU cycles). For 128 audio frames ≈ 2.9ms ≈ 1.49 frame seq ticks.

GB Square 1 registers:
- `$FF10` (NR10): sweep (write `$00` to disable)
- `$FF11` (NR11): duty (bits 7-6) + length (bits 5-0)
- `$FF12` (NR12): volume (bits 7-4) + direction (bit 3) + period (bits 2-0)
- `$FF13` (NR13): frequency low 8 bits
- `$FF14` (NR14): trigger (bit 7) + freq high (bits 2-0)

MIDI note to GB frequency: `gb_freq = 2048 - (131072 / freq)` where `freq = 440 * 2^((note-69)/12)`

Enable sound: write `$FF26` (NR52) = `$80` (master enable), `$FF25` (NR51) = `$FF` (all channels both sides), `$FF24` (NR50) = `$77` (max volume both sides).

**Step 2: Add chip enum and gb_apu to instance**

```cpp
extern "C" {
#include "gb_apu.h"  // or whatever the include path is
}

enum { CHIP_NES = 0, CHIP_GB = 1 };

typedef struct {
    char module_dir[256];

    // APU engines
    Simple_Apu nes_apu;
    struct gb_apu gb_apu;
    uint8_t chip;  // CHIP_NES or CHIP_GB

    // ... rest of fields
} chiptune_instance_t;
```

**Step 3: Initialize GB APU in create_instance**

```cpp
gb_apu_init(&inst->gb_apu, false);  // C blip_buf mode
gb_apu_set_sample_rate(&inst->gb_apu, 44100);
// Enable sound system
gb_apu_bus_write(&inst->gb_apu, 0xFF26, 0x80); // NR52: master on
gb_apu_bus_write(&inst->gb_apu, 0xFF25, 0xFF); // NR51: all channels both sides
gb_apu_bus_write(&inst->gb_apu, 0xFF24, 0x77); // NR50: max volume
```

**Step 4: Add MIDI handling for GB mode**

On note-on when `chip == CHIP_GB`:
```cpp
float freq = 440.0f * powf(2.0f, (note - 69) / 12.0f);
int gb_freq = (int)(2048.0f - 131072.0f / freq);
if (gb_freq < 0) gb_freq = 0;
if (gb_freq > 2047) gb_freq = 2047;

uint8_t vol = (data2 * 15) / 127;
gb_apu_bus_write(&inst->gb_apu, 0xFF10, 0x00);          // NR10: no sweep
gb_apu_bus_write(&inst->gb_apu, 0xFF11, 0x80);          // NR11: 50% duty
gb_apu_bus_write(&inst->gb_apu, 0xFF12, (vol << 4));    // NR12: volume, no envelope
gb_apu_bus_write(&inst->gb_apu, 0xFF13, gb_freq & 0xFF); // NR13: freq low
gb_apu_bus_write(&inst->gb_apu, 0xFF14, 0x80 | ((gb_freq >> 8) & 0x07)); // NR14: trigger + freq high
```

**Step 5: Add GB render path in render_block**

```cpp
if (inst->chip == CHIP_GB) {
    // Clock frame sequencer (~1-2 times per 128-frame block)
    long total_cycles = (long)(4194304.0 / 44100.0 * frames);
    // Run in chunks, triggering frame sequencer at 512 Hz intervals
    long cycles_per_fs = 4194304 / 512;  // 8192
    long remaining = total_cycles;
    while (remaining > 0) {
        long chunk = remaining < cycles_per_fs ? remaining : cycles_per_fs;
        gb_apu_run(&inst->gb_apu, chunk);
        gb_apu_frame_sequencer_clock(&inst->gb_apu);
        remaining -= chunk;
    }
    // Read samples
    int avail = gb_apu_samples_avail(&inst->gb_apu);
    int16_t mono[256];
    int count = gb_apu_read_samples(&inst->gb_apu, mono, avail < 256 ? avail : 256);
    // Mono to stereo
    for (int i = 0; i < count && i < frames; i++) {
        int32_t s = mono[i] * 4;
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        out_interleaved_lr[i * 2] = (int16_t)s;
        out_interleaved_lr[i * 2 + 1] = (int16_t)s;
    }
} else {
    // NES path (existing)
}
```

**Step 6: Add "chip" parameter to set_param/get_param**

- `set_param("chip", "NES")` → `inst->chip = CHIP_NES`
- `set_param("chip", "GB")` → `inst->chip = CHIP_GB`
- `get_param("chip")` → returns "NES" or "GB"

**Step 7: Update build.sh to compile gb_apu sources**

Inspect the submodule to find exact file paths. Expected:
```bash
# Compile GB APU library
GB_SRCS="
    src/libs/gb_apu/src/gb_apu.c
    src/libs/gb_apu/src/blip_wrap.c
    src/libs/gb_apu/src/blip_buf.c
"
for src in $GB_SRCS; do
    obj="build/$(basename "$src" .c).o"
    ${CROSS_PREFIX}gcc -g -O3 -fPIC \
        -I src/libs/gb_apu/include \
        -c "$src" -o "$obj"
done
```

Add GB objects to the link step.

NOTE: The exact file layout of gb_apu may differ. Check `src/libs/gb_apu/` after submodule init.

**Step 8: Build, deploy, test**

- Build: verify compilation succeeds
- Deploy: install on device
- Test: play notes, switch chip parameter between NES and GB, verify both produce sound

**Step 9: Commit**

```bash
git add -A
git commit -m "feat: Game Boy APU integration - both NES and GB produce sound"
```

---

### Task 5: MIDI-to-Register Mapping for All Channels

**Files:**
- Create: `move-anything-chiptune/src/dsp/midi_to_apu.h`
- Modify: `move-anything-chiptune/src/dsp/chiptune_plugin.cpp`

**Goal:** Complete frequency tables and register write functions for all channels on both chips.

**Step 1: Create midi_to_apu.h with frequency conversion functions**

```cpp
#ifndef MIDI_TO_APU_H
#define MIDI_TO_APU_H

#include <math.h>
#include <stdint.h>

// MIDI note to frequency
static inline float midi_to_freq(int note) {
    return 440.0f * powf(2.0f, (note - 69) / 12.0f);
}

// NES: frequency to 11-bit period (pulse & triangle)
// CPU clock = 1789773 Hz (NTSC)
// Pulse period: f = CPU / (16 * (period + 1))
// Triangle period: f = CPU / (32 * (period + 1))
static inline int freq_to_nes_pulse_period(float freq) {
    if (freq < 1.0f) return 0x7FF;
    int p = (int)(1789773.0f / (16.0f * freq) - 1.0f + 0.5f);
    if (p < 0) p = 0;
    if (p > 0x7FF) p = 0x7FF;
    return p;
}

static inline int freq_to_nes_tri_period(float freq) {
    if (freq < 1.0f) return 0x7FF;
    int p = (int)(1789773.0f / (32.0f * freq) - 1.0f + 0.5f);
    if (p < 0) p = 0;
    if (p > 0x7FF) p = 0x7FF;
    return p;
}

// GB: frequency to 11-bit register value (square & wave)
// Square: f = 131072 / (2048 - reg)
// Wave: f = 65536 / (2048 - reg)
static inline int freq_to_gb_square_reg(float freq) {
    if (freq < 64.0f) return 0;  // Below range
    int r = (int)(2048.0f - 131072.0f / freq + 0.5f);
    if (r < 0) r = 0;
    if (r > 2047) r = 2047;
    return r;
}

static inline int freq_to_gb_wave_reg(float freq) {
    if (freq < 32.0f) return 0;
    int r = (int)(2048.0f - 65536.0f / freq + 0.5f);
    if (r < 0) r = 0;
    if (r > 2047) r = 2047;
    return r;
}

// NES noise period lookup (16 values, index 0 = highest pitch)
static const uint8_t nes_noise_periods[16] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
};

// Map MIDI note to NES noise period (higher notes = lower period index = higher pitch)
static inline int midi_to_nes_noise_period(int note) {
    int idx = 15 - (note - 36) / 6;  // Map ~36-127 to 15-0
    if (idx < 0) idx = 0;
    if (idx > 15) idx = 15;
    return idx;
}

// Map MIDI note to GB noise (shift + divisor)
static inline uint8_t midi_to_gb_noise_reg(int note) {
    // Higher notes = higher frequency = lower shift value
    int shift = 14 - (note - 36) / 7;
    if (shift < 0) shift = 0;
    if (shift > 14) shift = 14;
    int divisor = 1;  // divisor code 1
    return (uint8_t)((shift << 4) | divisor);
}

#endif /* MIDI_TO_APU_H */
```

**Step 2: Define channel types and register write helper functions**

In chiptune_plugin.cpp, add NES register write helpers:
- `nes_write_pulse(inst, channel, period, duty, volume)` — writes $4000-$4003 or $4004-$4007
- `nes_write_triangle(inst, period)` — writes $4008, $400A, $400B
- `nes_write_noise(inst, period_idx, mode, volume)` — writes $400C-$400F
- `nes_silence_channel(inst, channel)` — set volume to 0

Add GB register write helpers:
- `gb_write_square(inst, channel, freq_reg, duty, volume)` — writes channel 1 or 2 registers
- `gb_write_wave(inst, freq_reg, volume)` — writes $FF1A-$FF1E
- `gb_write_noise(inst, noise_reg, mode, volume)` — writes $FF20-$FF23
- `gb_silence_channel(inst, channel)` — set volume to 0

**Step 3: Enable all channels on both chips**

NES: `$4015 = 0x0F` (pulse1 + pulse2 + triangle + noise)
GB: Already enabled via NR52/NR51/NR50

**Step 4: Test each channel type**

Temporarily hardcode note-on to cycle through channels to verify each one sounds correct.

**Step 5: Commit**

```bash
git add -A
git commit -m "feat: complete MIDI-to-register mapping for all NES and GB channels"
```

---

### Task 6: Voice Allocator

**Files:**
- Create: `move-anything-chiptune/src/dsp/voice_alloc.h`
- Modify: `move-anything-chiptune/src/dsp/chiptune_plugin.cpp`

**Goal:** Paraphonic voice allocator that assigns MIDI notes to hardware channels with Auto/Lead/Locked modes.

**Step 1: Define voice and channel structures**

```cpp
#ifndef VOICE_ALLOC_H
#define VOICE_ALLOC_H

#include <stdint.h>

enum { CH_PULSE1 = 0, CH_PULSE2 = 1, CH_TRIANGLE = 2, CH_WAVE = 2, CH_NOISE = 3, CH_DPCM = 4 };
enum { ALLOC_AUTO = 0, ALLOC_LEAD = 1, ALLOC_LOCKED = 2 };
enum { CHAN_TYPE_PULSE = 0, CHAN_TYPE_TRIANGLE = 1, CHAN_TYPE_WAVE = 2, CHAN_TYPE_NOISE = 3 };

typedef struct {
    uint8_t active;       // voice is sounding
    uint8_t note;         // MIDI note number
    uint8_t velocity;     // MIDI velocity
    uint8_t channel_idx;  // which hardware channel (0-4)
    uint8_t channel_type; // CHAN_TYPE_*
    uint32_t age;         // for oldest-voice stealing
} voice_t;

#define MAX_HW_CHANNELS 5

typedef struct {
    voice_t voices[MAX_HW_CHANNELS];
    int num_channels;    // 5 for NES, 4 for GB
    uint32_t counter;    // monotonic age counter
    uint8_t alloc_mode;  // ALLOC_AUTO, ALLOC_LEAD, ALLOC_LOCKED
} voice_allocator_t;
```

**Step 2: Implement allocation functions**

- `voice_alloc_init(alloc, num_channels)` — reset all voices
- `voice_alloc_note_on(alloc, note, velocity, chip)` — returns channel index or -1
  - **Auto mode**: Find free channel. Prefer pulse for melody, noise for notes > 96. If none free, steal oldest.
  - **Lead mode**: Always use channel 0 (pulse 1). Kill any existing note.
  - **Locked mode**: Use channel assignment based on note range (configurable per preset).
- `voice_alloc_note_off(alloc, note)` — returns channel index of freed voice, or -1
- `voice_alloc_find_voice(alloc, note)` — find voice playing given note

**Step 3: Integrate allocator into on_midi**

Replace the single-channel MIDI handling with allocator calls:
```cpp
case 0x90: // Note On
    if (data2 > 0) {
        int ch = voice_alloc_note_on(&inst->alloc, note, data2, inst->chip);
        if (ch >= 0) {
            write_channel_note_on(inst, ch, note, data2);
        }
    } else {
        int ch = voice_alloc_note_off(&inst->alloc, note);
        if (ch >= 0) {
            write_channel_note_off(inst, ch);
        }
    }
    break;
case 0x80: // Note Off
    int ch = voice_alloc_note_off(&inst->alloc, note);
    if (ch >= 0) {
        write_channel_note_off(inst, ch);
    }
    break;
```

**Step 4: Add helper functions for channel-agnostic note on/off**

`write_channel_note_on(inst, ch, note, velocity)` — dispatches to the appropriate NES/GB register write function based on channel type.

`write_channel_note_off(inst, ch)` — silences the appropriate channel.

**Step 5: Build, deploy, test**

- Play single notes: should work as before
- Play chords: should hear multiple channels
- Play more notes than channels: oldest should be stolen

**Step 6: Commit**

```bash
git add -A
git commit -m "feat: paraphonic voice allocator with auto/lead/locked modes"
```

---

### Task 7: Software Envelopes and LFO

**Files:**
- Modify: `move-anything-chiptune/src/dsp/chiptune_plugin.cpp`

**Goal:** Add per-voice software ADSR envelopes and a global LFO for vibrato. The APU hardware envelopes are too limited (4-bit linear only), so we modulate APU volume registers from software envelopes at the render_block rate.

**Step 1: Define software envelope**

```cpp
typedef struct {
    float level;       // current level 0.0-1.0
    float attack;      // attack rate (0=instant, 1=slow)
    float decay;       // decay rate
    uint8_t stage;     // 0=idle, 1=attack, 2=decay, 3=sustain, 4=release
    uint8_t gate;      // gate on/off
} sw_envelope_t;
```

**Step 2: Implement envelope processing**

`sw_envelope_process(env, attack_param, decay_param)` — called once per render_block (every ~2.9ms):
- Attack: ramp level up to 1.0
- Decay: ramp level down to 0 (no sustain for chiptune — instant gate on/off is more authentic)
- Release: on gate off, ramp to 0

Map envelope level (0.0-1.0) to APU volume (0-15): `vol = (int)(level * 15)`

**Step 3: Implement LFO for vibrato**

```cpp
// In instance:
float lfo_phase;  // 0.0-1.0

// In render_block:
float lfo_rate = inst->vibrato_rate * 0.5f;  // Hz (0-7.5 Hz)
float lfo_inc = lfo_rate / 44100.0f * 128.0f; // phase increment per block
inst->lfo_phase += lfo_inc;
if (inst->lfo_phase >= 1.0f) inst->lfo_phase -= 1.0f;
float lfo_val = sinf(inst->lfo_phase * 2.0f * 3.14159f); // -1 to +1

// Apply to active voices
float vib_semitones = lfo_val * inst->vibrato_depth * 0.5f; // up to ±0.5 semitone
float freq_mult = powf(2.0f, vib_semitones / 12.0f);
```

**Step 4: Apply envelopes and vibrato in render_block**

Before clocking the APU each render_block:
1. For each active voice, process its envelope
2. Compute vibrato-modulated frequency
3. Write updated volume and frequency registers to APU
4. If envelope finished (level ≈ 0 and gate off), mark voice inactive

**Step 5: Add envelope/vibrato parameters**

- `env_attack` (0-15 mapped to time)
- `env_decay` (0-15 mapped to time)
- `vibrato_depth` (0-15)
- `vibrato_rate` (0-15)

**Step 6: Build, deploy, test**

- Notes should now have attack/decay shaping instead of instant on/off
- Add vibrato via knob — should hear pitch wobble

**Step 7: Commit**

```bash
git add -A
git commit -m "feat: software ADSR envelopes and LFO vibrato"
```

---

### Task 8: Preset System and Factory Presets

**Files:**
- Create: `move-anything-chiptune/src/dsp/chiptune_presets.h`
- Modify: `move-anything-chiptune/src/dsp/chiptune_plugin.cpp`

**Goal:** Define ~22 factory presets as compiled-in data. Implement preset browsing via get_param protocol.

**Step 1: Create chiptune_presets.h with preset data**

```cpp
#ifndef CHIPTUNE_PRESETS_H
#define CHIPTUNE_PRESETS_H

#include <stdint.h>

typedef struct {
    char name[32];
    uint8_t chip;           // CHIP_NES or CHIP_GB
    uint8_t alloc_mode;     // ALLOC_AUTO, ALLOC_LEAD, ALLOC_LOCKED
    uint8_t duty;           // 0-3 (12.5%, 25%, 50%, 75%)
    uint8_t env_attack;     // 0-15
    uint8_t env_decay;      // 0-15
    int8_t  sweep;          // -7..+7 (0=off)
    uint8_t vibrato_depth;  // 0-15
    uint8_t vibrato_rate;   // 0-15
    uint8_t noise_mode;     // 0=long, 1=short
    uint8_t wavetable_idx;  // GB wave table (0-7)
    uint8_t channel_mask;   // bitmask: which channels enabled
    int8_t  detune;         // cents detune for duo presets
    uint8_t volume;         // 0-15
} chiptune_preset_t;

static const chiptune_preset_t g_factory_presets[] = {
    // NES Presets
    {"NES Square Lead",   0, 0, 2, 0, 8,  0, 0, 0, 0, 0, 0x03, 0,  12},
    {"NES Bright Lead",   0, 0, 1, 0, 6,  0, 0, 0, 0, 0, 0x03, 0,  12},
    {"NES Thin Lead",     0, 0, 0, 0, 5,  0, 0, 0, 0, 0, 0x03, 0,  12},
    {"NES Duo Lead",      0, 0, 2, 0, 8,  0, 2, 5, 0, 0, 0x03, 10, 11},
    {"NES Triangle Bass", 0, 1, 2, 0, 10, 0, 0, 0, 0, 0, 0x04, 0,  15},
    {"NES Tri Sub",       0, 1, 2, 0, 12, 0, 0, 0, 0, 0, 0x04, 0,  15},
    {"NES Noise Hat",     0, 1, 2, 0, 2,  0, 0, 0, 1, 0, 0x08, 0,  10},
    {"NES Noise Snare",   0, 1, 2, 0, 5,  0, 0, 0, 0, 0, 0x08, 0,  12},
    {"NES Power Chord",   0, 0, 2, 0, 8,  0, 1, 4, 0, 0, 0x03, 0,  11},
    {"NES Full Kit",      0, 2, 2, 0, 8,  0, 0, 0, 0, 0, 0x0F, 0,  12},
    {"NES Vibrato Lead",  0, 1, 2, 0, 8,  0, 5, 6, 0, 0, 0x01, 0,  12},
    // GB Presets
    {"GB Square Lead",    1, 0, 2, 0, 8,  0, 0, 0, 0, 0, 0x03, 0,  12},
    {"GB Sweep Lead",     1, 1, 2, 0, 8,  3, 0, 0, 0, 0, 0x01, 0,  12},
    {"GB Pulse Duo",      1, 0, 2, 0, 8,  0, 0, 0, 0, 0, 0x03, 10, 11},
    {"GB Wave Bass",      1, 1, 2, 0, 10, 0, 0, 0, 0, 0, 0x04, 0,  15},
    {"GB Wave Pad",       1, 0, 2, 6, 12, 0, 2, 4, 0, 1, 0x04, 0,  12},
    {"GB Wave Growl",     1, 1, 2, 0, 6,  0, 0, 0, 0, 3, 0x04, 0,  13},
    {"GB Noise Hat",      1, 1, 2, 0, 2,  0, 0, 0, 1, 0, 0x08, 0,  10},
    {"GB Noise Snare",    1, 1, 2, 0, 5,  0, 0, 0, 0, 0, 0x08, 0,  12},
    {"GB Full Kit",       1, 2, 2, 0, 8,  0, 0, 0, 0, 0, 0x0F, 0,  12},
    {"GB Chiptune",       1, 2, 2, 0, 8,  0, 1, 4, 0, 0, 0x0F, 0,  11},
    {"GB Vibrato Lead",   1, 1, 2, 0, 8,  0, 5, 6, 0, 0, 0x01, 0,  12},
};

#define NUM_FACTORY_PRESETS (sizeof(g_factory_presets) / sizeof(g_factory_presets[0]))

#endif
```

**Step 2: Implement apply_preset function**

```cpp
static void apply_preset(chiptune_instance_t *inst, int idx) {
    const chiptune_preset_t *p = &g_factory_presets[idx];
    inst->chip = p->chip;
    inst->alloc.alloc_mode = p->alloc_mode;
    inst->duty = p->duty;
    inst->env_attack = p->env_attack;
    inst->env_decay = p->env_decay;
    inst->sweep = p->sweep;
    inst->vibrato_depth = p->vibrato_depth;
    inst->vibrato_rate = p->vibrato_rate;
    inst->noise_mode = p->noise_mode;
    inst->wavetable_idx = p->wavetable_idx;
    inst->channel_mask = p->channel_mask;
    inst->detune = p->detune;
    inst->volume = p->volume;
    strncpy(inst->preset_name, p->name, sizeof(inst->preset_name) - 1);

    // Re-initialize the appropriate APU
    if (inst->chip == CHIP_NES) {
        // Enable channels based on mask
        inst->nes_apu.write_register(0, 0x4015, inst->channel_mask & 0x1F);
    } else {
        gb_apu_bus_write(&inst->gb_apu, 0xFF26, 0x80);
        gb_apu_bus_write(&inst->gb_apu, 0xFF25, 0xFF);
        gb_apu_bus_write(&inst->gb_apu, 0xFF24, 0x77);
    }

    // Kill all voices
    voice_alloc_init(&inst->alloc, inst->chip == CHIP_NES ? 5 : 4);
    inst->alloc.alloc_mode = p->alloc_mode;
}
```

**Step 3: Wire up preset selection in set_param**

```cpp
if (strcmp(key, "preset") == 0) {
    int idx = atoi(val);
    if (idx >= 0 && idx < (int)NUM_FACTORY_PRESETS && idx != inst->current_preset) {
        inst->current_preset = idx;
        apply_preset(inst, idx);
    }
    return;
}
```

**Step 4: Update get_param for preset protocol**

- `"preset"` → current_preset index
- `"preset_count"` → NUM_FACTORY_PRESETS
- `"preset_name"` → current preset name

**Step 5: Define GB wavetables**

In chiptune_presets.h, add 8 built-in 32-sample wavetables:
```cpp
static const uint8_t gb_wavetables[8][16] = {
    {0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA, 0x99, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00}, // Sawtooth
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, // Square
    {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10}, // Triangle
    {0xF0, 0xE1, 0xD2, 0xC3, 0xB4, 0xA5, 0x96, 0x87, 0x78, 0x69, 0x5A, 0x4B, 0x3C, 0x2D, 0x1E, 0x0F}, // Sine-ish
    {0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00}, // Pulse
    {0xFD, 0xEC, 0xDB, 0xCA, 0xB9, 0xA8, 0x97, 0x86, 0x75, 0x64, 0x53, 0x42, 0x31, 0x20, 0x10, 0x00}, // Bass
    {0xFF, 0xDD, 0xBB, 0x99, 0xFF, 0xDD, 0xBB, 0x99, 0x77, 0x55, 0x33, 0x11, 0x77, 0x55, 0x33, 0x11}, // Growl
    {0xF0, 0xF0, 0x00, 0x00, 0xF0, 0xF0, 0x00, 0x00, 0xF0, 0x00, 0xF0, 0x00, 0x0F, 0x0F, 0x0F, 0x0F}, // Metallic
};
```

Load wavetable into GB wave RAM ($FF30-$FF3F) when changing wavetable_idx.

**Step 6: Build, deploy, test**

- Scroll through presets: verify names appear, sounds change
- NES presets vs GB presets should sound distinctly different
- Wavetable presets should have unique wave timbres

**Step 7: Commit**

```bash
git add -A
git commit -m "feat: factory presets with NES and GB sounds"
```

---

### Task 9: Full Signal Chain Integration

**Files:**
- Modify: `move-anything-chiptune/src/dsp/chiptune_plugin.cpp`
- Modify: `move-anything-chiptune/src/module.json`

**Goal:** Complete chain_params, ui_hierarchy, and state save/load for full Shadow UI integration.

**Step 1: Define parameter table using param_helper**

```cpp
enum {
    P_DUTY = 0, P_ENV_ATTACK, P_ENV_DECAY, P_SWEEP,
    P_VIB_DEPTH, P_VIB_RATE, P_NOISE_MODE, P_WAVETABLE,
    P_VOLUME, P_OCTAVE,
    P_COUNT
};

static const param_def_t g_params[] = {
    {"duty",           "Duty",     PARAM_TYPE_INT, P_DUTY,       0, 3},
    {"env_attack",     "Attack",   PARAM_TYPE_INT, P_ENV_ATTACK, 0, 15},
    {"env_decay",      "Decay",    PARAM_TYPE_INT, P_ENV_DECAY,  0, 15},
    {"sweep",          "Sweep",    PARAM_TYPE_INT, P_SWEEP,      -7, 7},
    {"vibrato_depth",  "Vib Depth",PARAM_TYPE_INT, P_VIB_DEPTH,  0, 15},
    {"vibrato_rate",   "Vib Rate", PARAM_TYPE_INT, P_VIB_RATE,   0, 15},
    {"noise_mode",     "Noise",    PARAM_TYPE_INT, P_NOISE_MODE, 0, 1},
    {"wavetable",      "Wave",     PARAM_TYPE_INT, P_WAVETABLE,  0, 7},
    {"volume",         "Volume",   PARAM_TYPE_INT, P_VOLUME,     0, 15},
    {"octave_transpose","Octave",  PARAM_TYPE_INT, P_OCTAVE,     -3, 3},
};
```

**Step 2: Implement chain_params in get_param**

Use param_helper_chain_params_json() plus manual entries for:
- `chip` (enum: NES, GB)
- `alloc_mode` (enum: Auto, Lead, Locked)
- `preset` (int, max from preset_count)

**Step 3: Implement ui_hierarchy in get_param**

```json
{
  "modes": null,
  "levels": {
    "root": {
      "list_param": "preset",
      "count_param": "preset_count",
      "name_param": "preset_name",
      "children": "main",
      "knobs": ["duty", "env_attack", "env_decay", "sweep", "vibrato_depth", "vibrato_rate", "noise_mode", "volume"],
      "params": []
    },
    "main": {
      "label": "Parameters",
      "children": null,
      "knobs": ["duty", "env_attack", "env_decay", "sweep", "vibrato_depth", "vibrato_rate", "noise_mode", "volume"],
      "params": [
        {"key": "chip", "label": "Chip"},
        {"key": "duty", "label": "Duty Cycle"},
        {"key": "env_attack", "label": "Attack"},
        {"key": "env_decay", "label": "Decay"},
        {"key": "sweep", "label": "Sweep"},
        {"key": "vibrato_depth", "label": "Vibrato Depth"},
        {"key": "vibrato_rate", "label": "Vibrato Rate"},
        {"key": "alloc_mode", "label": "Voice Mode"},
        {"key": "noise_mode", "label": "Noise Mode"},
        {"key": "wavetable", "label": "Wavetable (GB)"},
        {"key": "volume", "label": "Volume"},
        {"key": "octave_transpose", "label": "Octave"}
      ]
    }
  }
}
```

**Step 4: Implement state save/load**

`get_param("state")`:
```cpp
snprintf(buf, buf_len, "{\"preset\":%d,\"chip\":%d,\"alloc_mode\":%d,\"duty\":%d,...}",
    inst->current_preset, inst->chip, inst->alloc.alloc_mode, (int)inst->params[P_DUTY], ...);
```

`set_param("state", json)`:
- Parse JSON, restore preset first, then override individual params

**Step 5: Update module.json with ui_hierarchy in capabilities**

Add the full `ui_hierarchy` and `chain_params` sections to module.json (matching braids pattern where applicable).

**Step 6: Build, deploy, test**

- Load in Signal Chain: preset browser should work
- Knobs should control parameters
- Save/load patch: parameters should persist
- Drill into "Parameters" submenu: all params editable

**Step 7: Commit**

```bash
git add -A
git commit -m "feat: full Signal Chain integration with chain_params, ui_hierarchy, state save/load"
```

---

### Task 10: Chain Patches, Polish, and Final Testing

**Files:**
- Create: `move-anything-chiptune/src/chain_patches/Chiptune NES.json`
- Create: `move-anything-chiptune/src/chain_patches/Chiptune GB.json`
- Modify: `move-anything-chiptune/scripts/build.sh` (include chain_patches)
- Modify: `move-anything-chiptune/scripts/install.sh` (install chain_patches)

**Step 1: Create chain patch JSON files**

Look at existing chain patches in `move-anything/patches/` for the format, then create two starter patches:
- `Chiptune NES.json`: chiptune sound generator (preset 0, NES Square Lead) + freeverb audio FX
- `Chiptune GB.json`: chiptune sound generator (preset 12, GB Square Lead) + freeverb audio FX

**Step 2: Update build.sh to include chain patches in dist**

```bash
if [ -d "src/chain_patches" ]; then
    mkdir -p dist/chiptune/chain_patches
    for f in src/chain_patches/*.json; do
        [ -f "$f" ] && cat "$f" > "dist/chiptune/chain_patches/$(basename "$f")"
    done
fi
```

**Step 3: Update install.sh to deploy chain patches**

```bash
if [ -d "src/chain_patches" ] || [ -d "dist/chiptune/chain_patches" ]; then
    echo "Installing chain presets..."
    scp src/chain_patches/*.json ableton@move.local:/data/UserData/move-anything/patches/
fi
```

**Step 4: Final testing checklist**

- [ ] Module loads in Signal Chain without crash
- [ ] All NES presets produce appropriate sound
- [ ] All GB presets produce appropriate sound
- [ ] Preset browsing works (scroll through names)
- [ ] Knob 1-8 control parameters in real-time
- [ ] Playing chords uses multiple channels (paraphonic)
- [ ] Voice stealing works when exceeding channel count
- [ ] Vibrato audible on presets with vibrato_depth > 0
- [ ] Envelope shaping audible (not just instant on/off)
- [ ] State save/load persists across patch changes
- [ ] Chain patches load and play
- [ ] No audio glitches or crashes during extended play

**Step 5: Commit**

```bash
git add -A
git commit -m "feat: chain patches and polish - ready for release"
```

---

## Implementation Notes

### Key gotchas to watch for:

1. **Nes_Snd_Emu sample output level** — The library outputs relatively quiet samples. May need gain scaling (try 4x-8x). Clamp after scaling.

2. **gb_apu frame sequencer** — Must be clocked at 512 Hz for correct envelope/sweep behavior. If not clocked, envelopes won't work and sound will be wrong.

3. **GB wave channel** — Must disable channel ($FF1A bit 7 = 0) before writing wave RAM ($FF30-$FF3F), then re-enable. Writing while enabled causes corruption.

4. **NES triangle volume** — Triangle channel has no volume control. It's either on or off. Software envelope can only gate it, not fade.

5. **APU state between presets** — When switching presets (especially NES↔GB), fully reinitialize the APU to avoid leftover register state causing artifacts.

6. **Build order** — gb_apu is C, Nes_Snd_Emu is C++. The plugin wrapper is C++. Compile C files with `gcc`, C++ files with `g++`, link with `g++`.

7. **Simple_Apu timing** — `end_frame(cycles)` must be called with correct CPU cycle count. Too few cycles = not enough samples generated. Too many = buffer overflow. Match cycles to frames: `cycles = (long)(1789773.0 / 44100.0 * frames)`.
