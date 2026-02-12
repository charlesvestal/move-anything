#!/usr/bin/env python3
"""Realistic RTP-MIDI stress test: AUM-like patterns without zero-delay flood."""
import socket, struct, time, random, subprocess, sys

addr = sys.argv[1] if len(sys.argv) > 1 else "move.local"
try:
    addr = socket.getaddrinfo(addr, 5004, socket.AF_INET)[0][4][0]
except Exception as e:
    print(f"Resolve failed: {e}")
    sys.exit(1)

LOCAL_SSRC = random.randint(1, 0xFFFFFFFF)
TOKEN = random.randint(1, 0xFFFFFFFF)
SEQ = 0

cs = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
ds = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
cs.settimeout(3); ds.settimeout(3)

sig = struct.pack("!HHIII", 0xFFFF, 0x494E, 2, TOKEN, LOCAL_SSRC) + b"Test\x00"
cs.sendto(sig, (addr, 5004)); cs.recvfrom(2048)
ds.sendto(sig, (addr, 5005)); ds.recvfrom(2048)
print("Connected")
time.sleep(0.3)

def send_rtp_midi(midi_bytes):
    global SEQ
    midi_len = len(midi_bytes)
    if midi_len > 15:
        hdr = bytes([0x80 | ((midi_len >> 8) & 0x0F), midi_len & 0xFF])
    else:
        hdr = bytes([midi_len & 0x0F])
    rtp = struct.pack("!BBHI", 0x80, 0xE1, SEQ & 0xFFFF,
                       int(time.monotonic() * 10000) & 0xFFFFFFFF)
    rtp += struct.pack("!I", LOCAL_SSRC) + hdr + midi_bytes
    ds.sendto(rtp, (addr, 5005))
    SEQ += 1

def check_alive():
    print("  Move alive?", end=" ")
    try:
        r = subprocess.run(["ssh", "ableton@move.local",
            "ps aux | grep move-anything | grep -v grep | wc -l"],
            capture_output=True, text=True, timeout=10)
        count = r.stdout.strip()
        print(f"yes ({count} processes)" if int(count) > 0 else "NO - crashed!")
        return int(count) > 0
    except Exception as e:
        print(f"NO RESPONSE: {e}")
        return False

# Test 1: Rapid single notes (10ms spacing - like fast keyboard playing)
print("Test 1: 100 rapid single notes at 10ms...")
for i in range(100):
    note = 48 + (i % 24)
    send_rtp_midi(bytes([0x90, note, 100]))
    time.sleep(0.01)
    send_rtp_midi(bytes([0x80, note, 0]))
    time.sleep(0.01)
time.sleep(1)
if not check_alive():
    sys.exit(1)

# Test 2: 4-note chords (30ms spacing - like AUM sequencer)
print("Test 2: 50 rapid 4-note chords at 30ms...")
for i in range(50):
    base = 48 + (i % 12)
    send_rtp_midi(bytes([0x90, base, 100, 0x90, base+4, 90, 0x90, base+7, 80, 0x90, base+12, 70]))
    time.sleep(0.03)
    send_rtp_midi(bytes([0x80, base, 0, 0x80, base+4, 0, 0x80, base+7, 0, 0x80, base+12, 0]))
    time.sleep(0.03)
time.sleep(1)
if not check_alive():
    sys.exit(1)

# Test 3: Sustained polyphony (like holding chords, 100ms between)
print("Test 3: 20 sustained 6-note chords at 100ms...")
for i in range(20):
    base = 36 + (i % 12)
    for n in [base, base+3, base+7, base+10, base+14, base+17]:
        send_rtp_midi(bytes([0x90, n, 80]))
    time.sleep(0.1)
    for n in [base, base+3, base+7, base+10, base+14, base+17]:
        send_rtp_midi(bytes([0x80, n, 0]))
    time.sleep(0.05)
time.sleep(1)
if not check_alive():
    sys.exit(1)

# Test 4: CC bursts (like AUM knob automation)
print("Test 4: 200 CC messages at 5ms (knob twisting)...")
for i in range(200):
    cc_val = int(64 + 63 * (0.5 + 0.5 * (i % 20) / 20))
    send_rtp_midi(bytes([0xB0, 1, cc_val & 0x7F]))  # mod wheel
    time.sleep(0.005)
time.sleep(1)
if not check_alive():
    sys.exit(1)

# Cleanup
bye = struct.pack("!HHIII", 0xFFFF, 0x4259, 2, TOKEN, LOCAL_SSRC)
cs.sendto(bye, (addr, 5004))
cs.close(); ds.close()
print("All tests passed!")
