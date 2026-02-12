#!/usr/bin/env python3
"""Verify RTP-MIDI produces actual results on Move.
Run FROM Mac, orchestrates device-side checks via SSH."""
import socket, struct, time, random, subprocess, sys

addr = sys.argv[1] if len(sys.argv) > 1 else "move.local"
try:
    addr = socket.getaddrinfo(addr, 5004, socket.AF_INET)[0][4][0]
except Exception as e:
    print(f"Resolve failed: {e}"); sys.exit(1)

LOCAL_SSRC = random.randint(1, 0xFFFFFFFF)
TOKEN = random.randint(1, 0xFFFFFFFF)
SEQ = 0

cs = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
ds = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
cs.settimeout(3); ds.settimeout(3)

sig = struct.pack("!HHIII", 0xFFFF, 0x494E, 2, TOKEN, LOCAL_SSRC) + b"Test\x00"
cs.sendto(sig, (addr, 5004)); cs.recvfrom(2048)
ds.sendto(sig, (addr, 5005)); ds.recvfrom(2048)
print("RTP connected")
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

def ssh(cmd):
    r = subprocess.run(["ssh", "ableton@move.local", cmd],
                       capture_output=True, text=True, timeout=10)
    return r.stdout.strip()

# Enable MIDI logging so we can see dispatch
ssh("touch /data/UserData/move-anything/shadow_midi_log_on")
print("Enabled MIDI logging")

# Deploy a small Python script ON THE DEVICE to read SHM and MIDI_OUT
DEVICE_SCRIPT = r'''
import mmap, os, struct, time, sys

# Read shadow mailbox MIDI_OUT region via /proc
# The shadow_mailbox is a static array in the shim .so - can't easily read it from another process
# Instead, read shadow audio SHM to check for non-zero audio

# Check shadow audio SHM
try:
    fd = os.open("/dev/shm/move-shadow-audio", os.O_RDONLY)
    mm = mmap.mmap(fd, 1536, mmap.MAP_SHARED, mmap.PROT_READ)
    # Read all 1536 bytes (3 x 512 byte triple-buffered audio blocks)
    data = mm.read(1536)
    mm.close()
    os.close(fd)
    # Check for non-zero int16 samples
    samples = struct.unpack("<768h", data)
    nonzero = sum(1 for s in samples if abs(s) > 10)
    peak = max(abs(s) for s in samples)
    print(f"SHADOW_AUDIO: {nonzero}/768 samples > 10, peak={peak}")
except Exception as e:
    print(f"SHADOW_AUDIO: error - {e}")

# Check RTP MIDI SHM (is shim consuming?)
try:
    fd = os.open("/dev/shm/move-shadow-rtp-midi", os.O_RDONLY)
    mm = mmap.mmap(fd, 260, mmap.MAP_SHARED, mmap.PROT_READ)
    data = mm.read(260)
    mm.close()
    os.close(fd)
    write_idx = struct.unpack("<H", data[0:2])[0]
    ready = data[2]
    buf_nonzero = sum(1 for b in data[4:260] if b != 0)
    print(f"RTP_SHM: write_idx={write_idx} ready={ready} buf_nonzero={buf_nonzero}")
except Exception as e:
    print(f"RTP_SHM: error - {e}")
'''

# Write the check script to device
ssh(f"cat > /tmp/check_shm.py << 'PYEOF'\n{DEVICE_SCRIPT}\nPYEOF")

print("\n=== BASELINE (before sending notes) ===")
print(ssh("python3 /tmp/check_shm.py"))

print("\n=== Sending sustained chord (C E G, ch 1, 2 seconds) ===")
for note in [60, 64, 67]:
    send_rtp_midi(bytes([0x90, note, 100]))
    time.sleep(0.01)

time.sleep(0.5)
print("\n=== CHECK 1 (during notes, 0.5s in) ===")
print(ssh("python3 /tmp/check_shm.py"))

time.sleep(1.0)
print("\n=== CHECK 2 (during notes, 1.5s in) ===")
print(ssh("python3 /tmp/check_shm.py"))

# Release notes
for note in [60, 64, 67]:
    send_rtp_midi(bytes([0x80, note, 0]))
    time.sleep(0.01)

time.sleep(0.5)
print("\n=== CHECK 3 (after note-off, 0.5s later) ===")
print(ssh("python3 /tmp/check_shm.py"))

# Check debug log for MIDI dispatch messages
print("\n=== MIDI dispatch log ===")
log = ssh("grep -a 'midi_out:' /data/UserData/move-anything/debug.log | tail -10")
if log:
    print(log)
else:
    print("(no midi_out dispatch messages found)")

# Check for shadow_midi_forward log
log2 = ssh("cat /data/UserData/move-anything/shadow_midi_forward.log 2>/dev/null | tail -10")
if log2:
    print("\n=== MIDI forward log ===")
    print(log2)

# Check daemon log
print("\n=== RTP daemon log (last 10 lines) ===")
print(ssh("tail -10 /data/UserData/move-anything/rtpmidi.log"))

# Alive check
print("\n=== Move alive? ===")
count = ssh("ps aux | grep move-anything | grep -v grep | wc -l")
print(f"Processes: {count}")

# Clean up
ssh("rm -f /data/UserData/move-anything/shadow_midi_log_on")
bye = struct.pack("!HHIII", 0xFFFF, 0x4259, 2, TOKEN, LOCAL_SSRC)
cs.sendto(bye, (addr, 5004))
cs.close(); ds.close()
print("\nDone.")
