#!/usr/bin/env python3
"""Stress test RTP-MIDI: rapid chords to test for crashes."""
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

sig = struct.pack("!HHIII", 0xFFFF, 0x494E, 2, TOKEN, LOCAL_SSRC) + b"Stress\x00"
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

# Test 1: Rapid single notes (like fast keyboard playing)
print("Test 1: 100 rapid single notes...")
for i in range(100):
    note = 48 + (i % 24)
    send_rtp_midi(bytes([0x90, note, 100]))
    time.sleep(0.01)
    send_rtp_midi(bytes([0x80, note, 0]))
    time.sleep(0.01)
print("  Move alive?", end=" ")
r = subprocess.run(["ssh", "ableton@move.local", "echo yes"], capture_output=True, text=True, timeout=10)
print(r.stdout.strip() or "NO RESPONSE")

# Test 2: Chords (4 notes per packet)
print("Test 2: 50 rapid 4-note chords...")
for i in range(50):
    base = 48 + (i % 12)
    send_rtp_midi(bytes([0x90, base, 100, 0x90, base+4, 90, 0x90, base+7, 80, 0x90, base+12, 70]))
    time.sleep(0.03)
    send_rtp_midi(bytes([0x80, base, 0, 0x80, base+4, 0, 0x80, base+7, 0, 0x80, base+12, 0]))
    time.sleep(0.03)
print("  Move alive?", end=" ")
r = subprocess.run(["ssh", "ableton@move.local", "echo yes"], capture_output=True, text=True, timeout=10)
print(r.stdout.strip() or "NO RESPONSE")

# Test 3: Flood — no sleep between notes
print("Test 3: 200 notes with no delay...")
for i in range(200):
    note = 36 + (i % 48)
    send_rtp_midi(bytes([0x90, note, 100]))
    send_rtp_midi(bytes([0x80, note, 0]))
print("  Sleeping 2s...")
time.sleep(2)
print("  Move alive?", end=" ")
r = subprocess.run(["ssh", "ableton@move.local", "echo yes"], capture_output=True, text=True, timeout=10)
print(r.stdout.strip() or "NO RESPONSE")

# Cleanup
bye = struct.pack("!HHIII", 0xFFFF, 0x4259, 2, TOKEN, LOCAL_SSRC)
cs.sendto(bye, (addr, 5004))
cs.close(); ds.close()
print("Done.")
