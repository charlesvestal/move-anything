# Move Everything Manual

## What is Move Everything?

Move Everything adds four instrument slots that run in "shadow mode" alongside your normal Move tracks. Each slot can host synthesizers and effects that mix with Move's audio output.

**Key concepts:**
- **Shadow Mode**: A custom UI accessed via keyboard shortcuts
- **Slots**: Four instrument slots plus a Master FX slot
- **Modules**: Synthesizers, audio effects, and MIDI effects that run in each slot
- **Overtake Modules**: Full-screen applications that take over Move's UI (like MIDI controllers)

---

## Important Notice

This is an unofficial project that modifies the software on your Ableton Move. Back up any important sets and samples before installing. Familiarize yourself with Move's DFU restore mode (on Centercode) in case you need to restore your device.

Move still works normally after installation - Move Everything runs alongside it.

---

## Installation

### Prerequisites

1. Your Move must be connected to WiFi
2. You need a computer on the same network with SSH access

### Setup SSH Access

1. On your computer, generate an SSH key if you don't have one:
   ```
   ssh-keygen -t ed25519
   ```

2. Copy your public key (usually `~/.ssh/id_ed25519.pub`)

3. Open a browser and go to:
   ```
   http://move.local/development/ssh
   ```

4. Paste your public key and save

5. Test the connection:
   ```
   ssh ableton@move.local
   ```

### Install Move Everything

Run the installer:
```
curl -L https://raw.githubusercontent.com/charlesvestal/move-anything/main/scripts/install.sh | sh
```

The installer will ask if you want to install all available modules. Choose:
- **Yes** to download all synths and effects (recommended for first install)
- **No** to install just the core - you can add modules later via the Module Store

After installation, Move will restart automatically.

---

## Shortcuts

All shortcuts use **Shift + touch Volume knob** as a modifier:

| Shortcut | Action |
|----------|--------|
| **Shift+Vol + Track 1-4** | Open that slot's editor |
| **Shift+Vol + Menu** | Open Master FX |
| **Shift+Vol + Jog Click** | Open Overtake menu (or exit Overtake mode) |
| **Shift+Vol + Knob 8** | Open Standalone Mode |

**Tip:** You can access slots directly from normal Move mode - you don't need to be in shadow mode first.

---

## Instrument Slots

Each of the four instrument slots contains a signal chain:

```
MIDI FX → Sound Generator → Audio FX 1 → Audio FX 2
```

### Navigating a Slot

- **Jog wheel**: Scroll between chain positions (MIDI FX, Synth, FX1, FX2, Settings)
- **Jog click**: Enter the selected position
- **Back button**: Go back one level

### Selecting Modules

1. Navigate to an empty position and click the jog wheel
2. Choose from installed modules of that type
3. To change an existing module: hold **Shift** and click the jog wheel

### Using Modules

Most modules have:
- **Preset browser**: Use jog wheel to browse presets, click to select
- **Parameter menu**: Editable settings organized hierarchically
- **Knob assignments**: Knobs 1-8 control relevant parameters when a module is selected

---

## Slot Settings

The last position in each slot contains settings:

| Setting | Description |
|---------|-------------|
| **Knob 1-8** | Assign any module parameter to a knob. These work even in normal Move mode (hold Shift + turn knob). |
| **Volume** | Slot volume level |
| **Receive Ch** | MIDI channel this slot listens to (match your Move track's MIDI Out) |
| **Forward Ch** | MIDI channel sent to the synth module (use for multitimbral patches) |

### Slot Presets

- **Save / Save As**: Save the entire slot configuration (all modules + settings)
- **Delete**: Remove a saved preset
- **Load**: Scroll left to the slot overview, click to see saved presets

---

## Master FX

Access via **Shift+Vol + Menu**. Contains four audio effect slots that process the mixed output of all instrument slots.

---

## Connecting to Move Tracks

1. Set a Move track's **MIDI Out** to a channel (1-4)
2. Set the corresponding slot's **Receive Ch** to match
3. Play the Move track - its MIDI triggers the slot's synth

Move Everything also forwards pitch bend, mod wheel, sustain, and other CCs from external MIDI controllers.

---

## Overtake Modules

Overtake modules take full control of Move's display and controls. Access via **Shift+Vol + Jog Click**.

To exit an overtake module: **Shift+Vol + Jog Click** (works anytime)

---

## Built-in Modules

These modules are included with Move Everything:

**MIDI FX:**
- **Chords** - Chord generator with shapes, inversions, voicing, and strum
- **Arpeggiator** - Pattern-based arpeggiator (up, down, up/down, random)

**Audio FX:**
- **Freeverb** - Simple, effective reverb

**Overtake:**
- **MIDI Controller** - 16-bank MIDI controller with customizable knob/pad assignments

---

## Module Store

When selection a Module, "[Get more...]" will open the Module Store to download additional Modules of that type. To update Move Anything itself, access Module Store via Standalone Mode (**Shift+Vol + Knob 8**), then select Module Store.


**Sound Generators:**
- **Braids** - Mutable Instruments macro oscillator (47 algorithms)
- **Dexed** - 6-operator FM synthesizer (DX7 compatible)
- **SF2** - SoundFont player
- **Mini-JV** - Roland JV-880 emulation
- **OB-Xd** - Oberheim-style virtual analog

**Audio Effects:**
- **CloudSeed** - Shimmer reverb
- **PSXVerb** - PlayStation-style reverb
- **TapeDelay** - RE-201 Space Echo style delay
- **TAPESCAM** - Tape saturation/degradation

**Overtake:**
- **M8 LPP** - Launchpad Pro emulator for Dirtywave M8
- **SID Control** - Controller for SIDaster III

**Note:** Some modules require additional files (patches, ROMs, SoundFonts). Check each module's documentation.

---

## Standalone Mode

Access via **Shift+Vol + Knob 8**. Includes the full Module Store for downloading modules and updating Move Everything. Also lets you run modules without Move's audio.

---

## Multichannel Audio Streaming

Move Everything can stream 14 channels of audio to your Mac over the existing USB connection. This gives you individual stereo tracks for each slot, the Move Everything mix, native Move audio, and the combined output — all available as inputs in Ableton Live or any DAW.

### Channel Layout

| Channels | Content |
|----------|---------|
| 1-2 | Slot 1 L/R (pre-volume) |
| 3-4 | Slot 2 L/R (pre-volume) |
| 5-6 | Slot 3 L/R (pre-volume) |
| 7-8 | Slot 4 L/R (pre-volume) |
| 9-10 | ME Stereo Mix (post-volume, pre-Master FX) |
| 11-12 | Move Native (Move's own audio, without Move Everything) |
| 13-14 | Combined (Move + ME, post-Master FX) |

### One-Time Mac Setup

1. Install BlackHole (virtual audio device):
   ```
   brew install blackhole-16ch
   ```

2. Build the receiver:
   ```
   cc -O2 -o move_audio_recv tools/move_audio_recv.c \
      -framework CoreAudio -framework AudioToolbox -framework CoreFoundation
   ```

### Streaming to Ableton Live

**Important:** Start Ableton Live before running the receiver. The receiver detects the audio device's sample rate at startup and resamples automatically if needed (e.g., Live running at 48kHz). Starting in the wrong order may cause audio artifacts.

1. Open Ableton Live and set audio input to **BlackHole 16ch**

2. Start the receiver on your Mac:
   ```
   ./move_audio_recv
   ```

3. Create stereo tracks for the channels you want to record:
   - Track 1: Input **1/2** (Slot 1)
   - Track 2: Input **3/4** (Slot 2)
   - Track 3: Input **5/6** (Slot 3)
   - Track 4: Input **7/8** (Slot 4)
   - Track 5: Input **9/10** (ME Mix)
   - Track 6: Input **11/12** (Move Native)
   - Track 7: Input **13/14** (Combined)

4. Arm the tracks and play

The receiver shows connection status, packet rate, and buffer health. Press Ctrl+C to stop.

### Recording to WAV Files

You can record directly to WAV files without BlackHole:

```
# Record all 14 channels to a single WAV
./move_audio_recv --wav session.wav

# Record separate stereo WAVs per channel pair
./move_audio_recv --wav session.wav --split

# Record for a specific duration
./move_audio_recv --wav session.wav --duration 30
```

With `--split`, you get individual files: `session_slot1.wav`, `session_slot2.wav`, `session_slot3.wav`, `session_slot4.wav`, `session_me_mix.wav`, `session_move_native.wav`, `session_combined.wav`.

### Receiver Options

| Option | Description |
|--------|-------------|
| `--list-devices` | Show available audio output devices and sample rates |
| `--device <name>` | Output to a specific device (default: BlackHole 16ch) |
| `--wav <file>` | Record to WAV file instead of audio output |
| `--split` | With `--wav`, create separate stereo WAV per channel pair |
| `--duration <sec>` | Stop recording after specified seconds |

### Sample Rate

Move streams audio at 44100 Hz. If your DAW runs at a different sample rate (e.g., 48000 Hz), the receiver automatically resamples to match the output device. To ensure correct detection, always start the receiver **after** your DAW is running and has claimed the audio device.

### Notes

- Always start your DAW before the receiver so the correct sample rate is detected
- The stream daemon starts automatically when Move boots — no setup needed on the Move side
- Audio is streamed over the USB NCM network link (the same connection used for SSH)
- If you hear the audio through your speakers without Ableton, check Audio MIDI Setup for a Multi-Output Device that includes BlackHole
- The stream adds ~1-2ms of latency beyond what's inherent in the audio block size

---

## Tips

- Slot settings persist between sessions
- Use the Module Store (in standalone mode) to update Move Everything itself
- If something goes wrong, use Move's DFU restore mode to reset
