#!/usr/bin/env python3
"""End-to-end RTP-MIDI test: send notes, check Move echoes them back, check audio."""
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

# AppleMIDI handshake
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

# Clear debug log
ssh("echo '' > /data/UserData/move-anything/debug.log")

print("\n=== Sending C4 note-on ch1, holding 1s ===")
send_rtp_midi(bytes([0x90, 60, 100]))
time.sleep(1.0)
send_rtp_midi(bytes([0x80, 60, 0]))
time.sleep(0.5)

print("\n=== Sending chord (C E G) ch1, holding 1s ===")
for note in [60, 64, 67]:
    send_rtp_midi(bytes([0x90, note, 100]))
    time.sleep(0.01)
time.sleep(1.0)
for note in [60, 64, 67]:
    send_rtp_midi(bytes([0x80, note, 0]))
    time.sleep(0.01)
time.sleep(0.5)

# Check results
print("\n=== RESULTS ===")

# 1. Is Move alive?
alive = ssh("ps aux | grep MoveOriginal | grep -v grep | wc -l")
print(f"Move alive: {'YES' if alive.strip() == '1' else 'NO (CRASHED)'}")

# 2. Check debug log for RTP injection and MIDI OUT echoes
print("\n--- Debug log (MIDI related) ---")
log = ssh("grep -a -E 'rtp|midi_out|shadow_chain_dispatch|MIDI_OUT|cable.2' /data/UserData/move-anything/debug.log 2>/dev/null | tail -20")
if log:
    print(log)
else:
    print("(no MIDI log entries)")

# 3. Check RTP daemon log
print("\n--- RTP daemon log ---")
rtp_log = ssh("tail -15 /data/UserData/move-anything/rtpmidi.log")
if rtp_log:
    print(rtp_log)

# 4. Check shadow audio SHM
print("\n--- Shadow audio SHM ---")
audio_check = ssh("""python3 -c "
import mmap, os, struct
try:
    fd = os.open('/dev/shm/move-shadow-audio', os.O_RDONLY)
    mm = mmap.mmap(fd, 1536, mmap.MAP_SHARED, mmap.PROT_READ)
    data = mm.read(1536)
    mm.close(); os.close(fd)
    samples = struct.unpack('<768h', data)
    nonzero = sum(1 for s in samples if abs(s) > 10)
    peak = max(abs(s) for s in samples)
    print(f'samples>{10}: {nonzero}/768, peak={peak}')
except Exception as e:
    print(f'error: {e}')
" """)
print(f"Shadow audio: {audio_check}")

# Cleanup
bye = struct.pack("!HHIII", 0xFFFF, 0x4259, 2, TOKEN, LOCAL_SSRC)
cs.sendto(bye, (addr, 5004))
cs.close(); ds.close()
print("\nDone.")
