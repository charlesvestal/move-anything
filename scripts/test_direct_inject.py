#!/usr/bin/env python3
"""Direct MIDI injection test — bypasses RTP daemon entirely.
Writes USB-MIDI packets straight to /dev/shm/move-shadow-rtp-midi on device,
then checks debug log for MIDI OUT echoes from Move."""
import subprocess, sys, time

def ssh(cmd):
    r = subprocess.run(["ssh", "ableton@move.local", cmd],
                       capture_output=True, text=True, timeout=10)
    return r.stdout.strip()

# Enable MIDI out logging
ssh("touch /data/UserData/move-anything/shadow_midi_out_log_on")
ssh("touch /data/UserData/move-anything/debug_log_on")
ssh("echo '' > /data/UserData/move-anything/debug.log")

# Deploy a tiny injector script on device
INJECTOR = r'''
import mmap, os, struct, time, sys

SHM_PATH = "/dev/shm/move-shadow-rtp-midi"
fd = os.open(SHM_PATH, os.O_RDWR)
mm = mmap.mmap(fd, 260, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE)

def inject(packets):
    """Write USB-MIDI packets to SHM. Each packet is 4 bytes."""
    buf = b"".join(packets)
    mm.seek(4)  # skip header (write_idx + ready + reserved)
    mm.write(buf)
    # Set write_idx to total bytes written
    mm.seek(0)
    mm.write(struct.pack("<H", len(buf)))
    mm.flush()

# Note-on C4 ch1 velocity 100 (cable 2, CIN 0x9)
# USB-MIDI: [0x29, 0x90, 0x3C, 0x64]
print("Injecting note-on C4 ch1...")
inject([bytes([0x29, 0x90, 0x3C, 0x64])])
time.sleep(1.5)

# Note-off C4 ch1
print("Injecting note-off C4 ch1...")
inject([bytes([0x28, 0x80, 0x3C, 0x00])])
time.sleep(0.5)

# Chord: C E G on ch1
print("Injecting chord C-E-G ch1...")
inject([
    bytes([0x29, 0x90, 0x3C, 0x64]),
    bytes([0x29, 0x90, 0x40, 0x64]),
    bytes([0x29, 0x90, 0x43, 0x64]),
])
time.sleep(1.5)

# Release chord
print("Injecting chord release...")
inject([
    bytes([0x28, 0x80, 0x3C, 0x00]),
    bytes([0x28, 0x80, 0x40, 0x00]),
    bytes([0x28, 0x80, 0x43, 0x00]),
])
time.sleep(0.5)

mm.close()
os.close(fd)
print("Done injecting.")
'''

ssh(f"cat > /tmp/inject_midi.py << 'PYEOF'\n{INJECTOR}\nPYEOF")

print("Running direct MIDI injection on device...")
inject_out = ssh("python3 /tmp/inject_midi.py")
print(inject_out)

# Check results
print("\n=== Move alive? ===")
alive = ssh("ps aux | grep MoveOriginal | grep -v grep | wc -l")
print(f"{'YES' if alive.strip() == '1' else 'NO — CRASHED!'}")

print("\n=== Debug log (MIDI OUT echoes) ===")
log = ssh("grep -a 'midi_out' /data/UserData/move-anything/debug.log 2>/dev/null | tail -20")
if log:
    print(log)
else:
    print("(no midi_out entries — Move did NOT echo the notes)")

print("\n=== Debug log (all entries) ===")
log2 = ssh("tail -30 /data/UserData/move-anything/debug.log 2>/dev/null")
if log2:
    print(log2)
else:
    print("(empty)")
