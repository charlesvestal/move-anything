#!/usr/bin/env python3
"""Test script for RTP-MIDI daemon on Move.

Simulates an AppleMIDI client: establishes a session, sends MIDI notes,
and verifies the daemon doesn't crash.
"""

import socket
import struct
import time
import sys
import random

MOVE_HOST = "move.local"
CONTROL_PORT = 5004
DATA_PORT = 5005

LOCAL_SSRC = 0x12345678
INITIATOR_TOKEN = random.randint(0, 0xFFFFFFFF)

# AppleMIDI signature
SIG = 0xFFFF
CMD_IN = 0x494E  # Invitation
CMD_OK = 0x4F4B  # Accept
CMD_CK = 0x434B  # Clock sync

def send_invitation(sock, port, name="TestClient"):
    """Send AppleMIDI invitation."""
    name_bytes = name.encode('utf-8') + b'\x00'
    pkt = struct.pack("!HHII", SIG, CMD_IN, 2, INITIATOR_TOKEN)
    pkt += struct.pack("!I", LOCAL_SSRC)
    pkt += name_bytes
    sock.sendto(pkt, (MOVE_HOST, port))
    print(f"  Sent IN to port {port}")

def recv_response(sock, timeout=3.0):
    """Receive and parse AppleMIDI response."""
    sock.settimeout(timeout)
    try:
        data, addr = sock.recvfrom(1024)
        if len(data) >= 8:
            sig, cmd = struct.unpack("!HH", data[:4])
            if sig == SIG:
                return cmd, data, addr
        return None, data, addr
    except socket.timeout:
        return None, None, None

def build_rtp_midi_packet(seq, midi_bytes, ssrc=LOCAL_SSRC):
    """Build an RTP-MIDI packet containing MIDI data."""
    timestamp = int(time.time() * 1000) & 0xFFFFFFFF

    # RTP header: V=2, P=0, X=0, CC=0, M=1, PT=97
    rtp_header = struct.pack("!BBHII",
        0x80,       # V=2, P=0, X=0, CC=0
        0x61 | 0x80,  # M=1, PT=97
        seq & 0xFFFF,
        timestamp,
        ssrc
    )

    # MIDI command section: B=0, J=0, Z=0, P=0, LEN=len(midi_bytes)
    midi_len = len(midi_bytes)
    if midi_len < 16:
        # Short header (1 byte): B=0, J=0, Z=0, P=0, LEN in bits 3-0
        cmd_header = struct.pack("B", midi_len & 0x0F)
    else:
        # Long header (2 bytes): B=1, J=0, Z=0, P=0, LEN in bits 11-0
        cmd_header = struct.pack("!H", 0x8000 | (midi_len & 0x0FFF))

    return rtp_header + cmd_header + bytes(midi_bytes)

def send_note_on(data_sock, seq, note, velocity=100, channel=0):
    """Send a Note On MIDI message via RTP-MIDI."""
    midi = [0x90 | channel, note, velocity]
    pkt = build_rtp_midi_packet(seq, midi)
    data_sock.sendto(pkt, (MOVE_HOST, DATA_PORT))
    return seq + 1

def send_note_off(data_sock, seq, note, channel=0):
    """Send a Note Off MIDI message via RTP-MIDI."""
    midi = [0x80 | channel, note, 0]
    pkt = build_rtp_midi_packet(seq, midi)
    data_sock.sendto(pkt, (MOVE_HOST, DATA_PORT))
    return seq + 1

def send_cc(data_sock, seq, cc, value, channel=0):
    """Send a CC MIDI message via RTP-MIDI."""
    midi = [0xB0 | channel, cc, value]
    pkt = build_rtp_midi_packet(seq, midi)
    data_sock.sendto(pkt, (MOVE_HOST, DATA_PORT))
    return seq + 1

def main():
    print("=== RTP-MIDI Test ===")
    print(f"Target: {MOVE_HOST}:{CONTROL_PORT}/{DATA_PORT}")
    print()

    # Create UDP sockets
    ctrl_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    data_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    ctrl_sock.bind(('', 0))
    data_sock.bind(('', 0))

    # Step 1: Session establishment
    print("1. Establishing session...")

    # Send invitation on control port
    send_invitation(ctrl_sock, CONTROL_PORT)
    cmd, data, addr = recv_response(ctrl_sock)
    if cmd == CMD_OK:
        remote_ssrc = struct.unpack("!I", data[8:12])[0]
        print(f"  Control OK (remote SSRC=0x{remote_ssrc:08X})")
    else:
        print(f"  Control invitation failed (cmd={cmd})")
        sys.exit(1)

    # Send invitation on data port
    send_invitation(data_sock, DATA_PORT)
    cmd, data, addr = recv_response(data_sock)
    if cmd == CMD_OK:
        print("  Data OK")
    else:
        print(f"  Data invitation failed (cmd={cmd})")
        sys.exit(1)

    print("  Session CONNECTED")
    print()

    # Step 2: Send test MIDI
    seq = 1

    print("2. Sending single note (C4)...")
    seq = send_note_on(data_sock, seq, 60, 100)
    time.sleep(0.3)
    seq = send_note_off(data_sock, seq, 60)
    time.sleep(0.2)
    print("  Done")

    print("3. Sending chord (C major)...")
    for note in [60, 64, 67]:
        seq = send_note_on(data_sock, seq, note, 80)
        time.sleep(0.01)
    time.sleep(0.5)
    for note in [60, 64, 67]:
        seq = send_note_off(data_sock, seq, note)
        time.sleep(0.01)
    time.sleep(0.2)
    print("  Done")

    print("4. Rapid notes (stress test)...")
    for i in range(20):
        note = 48 + (i % 24)
        seq = send_note_on(data_sock, seq, note, 90)
        time.sleep(0.02)
        seq = send_note_off(data_sock, seq, note)
        time.sleep(0.02)
    time.sleep(0.2)
    print("  Done")

    print("5. Sending CC messages...")
    for cc_val in range(0, 127, 10):
        seq = send_cc(data_sock, seq, 1, cc_val)  # Mod wheel
        time.sleep(0.02)
    time.sleep(0.2)
    print("  Done")

    print("6. Overlapping notes...")
    seq = send_note_on(data_sock, seq, 60, 100)
    time.sleep(0.05)
    seq = send_note_on(data_sock, seq, 64, 100)
    time.sleep(0.05)
    seq = send_note_on(data_sock, seq, 67, 100)
    time.sleep(0.3)
    seq = send_note_off(data_sock, seq, 60)
    time.sleep(0.05)
    seq = send_note_off(data_sock, seq, 64)
    time.sleep(0.05)
    seq = send_note_off(data_sock, seq, 67)
    time.sleep(0.2)
    print("  Done")

    print()
    print(f"=== Test complete ({seq-1} packets sent) ===")
    print("Check Move for crashes and verify audio output.")

    ctrl_sock.close()
    data_sock.close()

if __name__ == "__main__":
    main()
