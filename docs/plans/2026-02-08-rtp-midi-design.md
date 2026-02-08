# RTP-MIDI Support Design

## Goal

Enable wireless MIDI connectivity to Move via RTP-MIDI (Apple MIDI), so that a computer or phone on the same WiFi network can send MIDI to the Move without a cable.

## Architecture

```
Phone/Computer                          Move (WiFi)
+-----------+     RTP-MIDI (UDP)     +------------------------+
| iOS/macOS | --------------------->  | rtpmidi-daemon          |
| DAW / App |   ports 5004/5005      |   +-- mDNS advertise    |
+-----------+                        |   +-- AppleMIDI session  |
                                     |   +-- RTP-MIDI parse     |
                                     |   +-- shm write          |
                                     |        | (cable 2 pkts)  |
                                     | +--------------------+   |
                                     | | /move-shadow-rtp-   |   |
                                     | |  midi (shm)         |   |
                                     | +--------------------+   |
                                     |        |                 |
                                     | Shim merges into         |
                                     | MIDI_IN mailbox          |
                                     |        |                 |
                                     | Move / Move Anything     |
                                     +------------------------+
```

Incoming RTP-MIDI appears as cable 2 (external USB MIDI) in the hardware mailbox. Both stock Move and Move Anything see it identically to a USB MIDI device on USB-A.

## Components

### 1. RTP-MIDI Daemon (`src/rtpmidi/`)

Standalone C program, cross-compiled for ARM. ~500-800 lines.

**mDNS Advertisement:**
- Advertises `_apple-midi._udp` service so iOS/macOS/DAWs discover it automatically
- Uses Avahi if available, otherwise raw mDNS responder on port 5353
- Service name: "Move" (or configurable)

**AppleMIDI Session Protocol (UDP port 5004 control, 5005 data):**
- Handles IN (invitation) -> responds OK
- Handles CK (sync) -> timestamp exchange for clock sync
- Handles BY (bye) -> teardown
- Supports one active session (single client at a time)

**RTP-MIDI Packet Parsing:**
- Standard RTP header (12 bytes)
- MIDI command section: flags byte + delta-time compressed MIDI messages
- Journal section: skipped (local WiFi = minimal packet loss)
- Extracts raw MIDI bytes (note on/off, CC, pitch bend, etc.)

**Shared Memory Output:**
- Opens/creates `/move-shadow-rtp-midi` shared memory segment
- Writes USB-MIDI formatted packets (4 bytes each):
  - Byte 0: `[cable=2:4][CIN:4]` (CIN derived from MIDI status type)
  - Bytes 1-3: MIDI status, data1, data2
- Uses a ring buffer with ready counter (same pattern as `shadow_midi_out_shm`)

### 2. Shim Integration (`move_anything_shim.c`)

~50 lines of new code, following the `shadow_inject_ui_midi_out` pattern.

**New shared memory segment:**
- Opens `/move-shadow-rtp-midi` on startup
- Structure: `{ uint32_t ready; uint32_t write_idx; uint8_t buffer[256]; }`

**New function `shadow_inject_rtp_midi()`:**
- Called in the ioctl handler after hardware MIDI_IN is copied to shadow_mailbox
- Reads packets from the shm ring buffer
- Writes them into empty slots (zero'd 4-byte groups) at `shadow_mailbox + MIDI_IN_OFFSET`
- Clears the shm buffer after processing

**Daemon lifecycle:**
- On startup: if `rtpmidi_enabled` in config, fork/exec the daemon
- On toggle: start/stop daemon via SIGTERM
- On shutdown: kill daemon

### 3. UI Toggle (Master FX Settings)

Add to `MASTER_FX_SETTINGS_ITEMS_BASE` in `shadow_ui.js`:

```javascript
{ key: "rtpmidi_enabled", label: "RTP-MIDI", type: "bool" }
```

**getMasterFxSettingValue:** Return "On"/"Off" based on state variable.

**adjustMasterFxSetting:** Toggle the boolean, call shim to start/stop daemon, persist to `shadow_config.json`.

**Persistence:** Read/write `rtpmidi_enabled` in `shadow_config.json` alongside existing settings like `auto_update_check`.

### 4. Build Integration

- Add `src/rtpmidi/` to the Docker cross-compilation build
- Install `rtpmidi-daemon` binary alongside the host binary
- No new library dependencies (pure POSIX sockets + mDNS)

## Protocol Details

### AppleMIDI Command Packets (UDP)

```
Signature: 0xFF 0xFF (2 bytes)
Command:   "IN" / "OK" / "NO" / "BY" / "CK" (2 bytes)
Protocol:  uint32 (version 2)
Initiator: uint32 (token)
SSRC:      uint32 (sender identifier)
Name:      null-terminated string (for IN/OK only)
```

### RTP-MIDI Data Packets (UDP)

```
RTP Header (12 bytes):
  V=2, P=0, X=0, CC=0, M=0, PT=97
  Sequence number (16-bit)
  Timestamp (32-bit)
  SSRC (32-bit)

MIDI Command Section:
  Flags byte:
    Bit 7 (B): 1 = long header (12-bit length), 0 = short (4-bit)
    Bit 6 (J): journal present
    Bit 5 (Z): first delta-time is zero
    Bit 4 (P): phantom/status
    Bits 0-3/0-11: MIDI list length
  [Delta-time bytes...] (variable length)
  [MIDI bytes...]

Journal Section: (skipped - not parsed)
```

### USB-MIDI Packet Format (what we inject)

```
Byte 0: 0x2N where N = CIN
  CIN 0x8 = Note Off
  CIN 0x9 = Note On
  CIN 0xB = Control Change
  CIN 0xC = Program Change
  CIN 0xD = Channel Pressure
  CIN 0xE = Pitch Bend
Byte 1: MIDI status byte
Byte 2: MIDI data byte 1
Byte 3: MIDI data byte 2
```

## Constraints

- No ALSA on the device
- ARM Linux (cross-compiled via Docker)
- Must not impact audio thread latency (daemon is separate process, shm injection is lockfree)
- 64-slot MIDI buffer (256 bytes / 4) - adequate for musical MIDI rates
- WiFi latency: typically 1-5ms on local network, acceptable for musical use

## Testing

1. Build daemon, deploy to Move
2. Enable RTP-MIDI in Master FX Settings
3. On macOS: Audio MIDI Setup > Network > connect to "Move"
4. On iOS: any RTP-MIDI capable app should discover "Move"
5. Send MIDI notes, verify they arrive in Move Anything signal chain
6. Verify stock Move also receives the MIDI (when not in shadow mode)
