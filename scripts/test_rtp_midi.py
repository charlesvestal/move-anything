#!/usr/bin/env python3
"""
Test RTP-MIDI client that connects to Move's RTP-MIDI daemon and sends notes.
Usage: python3 scripts/test_rtp_midi.py [move-hostname]

Performs AppleMIDI session handshake on ports 5004/5005, then sends
a C major scale as RTP-MIDI packets. Prints each step for debugging.
"""

import socket
import struct
import time
import sys
import random

APPLEMIDI_SIG = 0xFFFF
CMD_IN = 0x494E    # 'IN'
CMD_OK = 0x4F4B    # 'OK'
CMD_BY = 0x4259    # 'BY'
RTP_VERSION = 2
RTP_PAYLOAD_TYPE = 0x61  # 97

HOST = sys.argv[1] if len(sys.argv) > 1 else "move.local"
CONTROL_PORT = 5004
DATA_PORT = 5005

LOCAL_SSRC = random.randint(1, 0xFFFFFFFF)
TOKEN = random.randint(1, 0xFFFFFFFF)
SEQ = 0


def send_invitation(sock, port, addr):
    """Send AppleMIDI IN (invitation) packet."""
    pkt = struct.pack('!HHIII',
        APPLEMIDI_SIG, CMD_IN,
        2, TOKEN, LOCAL_SSRC
    ) + b'TestClient\x00'
    sock.sendto(pkt, (addr, port))
    print(f"  -> Sent IN to {addr}:{port}")


def wait_for_ok(sock, timeout=3.0):
    """Wait for AppleMIDI OK response."""
    sock.settimeout(timeout)
    try:
        data, addr = sock.recvfrom(2048)
        if len(data) >= 4:
            sig, cmd = struct.unpack('!HH', data[:4])
            if sig == APPLEMIDI_SIG and cmd == CMD_OK:
                remote_ssrc = struct.unpack('!I', data[12:16])[0] if len(data) >= 16 else 0
                print(f"  <- Got OK from {addr} (SSRC=0x{remote_ssrc:08X})")
                return remote_ssrc
            else:
                print(f"  <- Got unexpected: sig=0x{sig:04X} cmd=0x{cmd:04X}")
        return None
    except socket.timeout:
        print(f"  <- Timeout waiting for OK")
        return None


def build_rtp_midi_packet(midi_bytes, seq, timestamp):
    """Build an RTP-MIDI packet containing the given MIDI bytes."""
    # RTP header: V=2, P=0, X=0, CC=0, M=1, PT=97
    rtp_header = struct.pack('!BBHI',
        (RTP_VERSION << 6),       # V=2, P=0, X=0, CC=0
        0x80 | RTP_PAYLOAD_TYPE,  # M=1, PT=97
        seq & 0xFFFF,
        timestamp
    )
    rtp_header += struct.pack('!I', LOCAL_SSRC)

    # MIDI command section
    midi_len = len(midi_bytes)
    if midi_len > 15:
        # B flag = 1: 2-byte length
        flags = 0x80 | ((midi_len >> 8) & 0x0F)
        midi_header = bytes([flags, midi_len & 0xFF])
    else:
        # B flag = 0: 1-byte length
        midi_header = bytes([midi_len & 0x0F])

    return rtp_header + midi_header + midi_bytes


def send_note(data_sock, addr, note, velocity, channel=0):
    """Send a note-on or note-off via RTP-MIDI."""
    global SEQ
    status = (0x90 | channel) if velocity > 0 else (0x80 | channel)
    midi = bytes([status, note, velocity])
    pkt = build_rtp_midi_packet(midi, SEQ, int(time.monotonic() * 10000) & 0xFFFFFFFF)
    data_sock.sendto(pkt, (addr, DATA_PORT))
    name = "ON " if velocity > 0 else "OFF"
    print(f"  -> Note {name}: note={note} vel={velocity} ch={channel} seq={SEQ}")
    SEQ += 1


def main():
    print(f"=== RTP-MIDI Test Client ===")
    print(f"Target: {HOST}:{CONTROL_PORT}/{DATA_PORT}")
    print(f"Local SSRC: 0x{LOCAL_SSRC:08X}")
    print()

    # Resolve hostname
    try:
        addr = socket.getaddrinfo(HOST, CONTROL_PORT, socket.AF_INET)[0][4][0]
        print(f"Resolved {HOST} -> {addr}")
    except Exception as e:
        print(f"Failed to resolve {HOST}: {e}")
        return 1

    # Create sockets
    control_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    data_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    # Step 1: Control port invitation
    print(f"\n[1] Control port handshake ({CONTROL_PORT})...")
    send_invitation(control_sock, CONTROL_PORT, addr)
    remote_ssrc = wait_for_ok(control_sock)
    if remote_ssrc is None:
        print("FAILED: No OK on control port")
        return 1

    # Step 2: Data port invitation
    print(f"\n[2] Data port handshake ({DATA_PORT})...")
    send_invitation(data_sock, DATA_PORT, addr)
    wait_for_ok(data_sock)

    time.sleep(0.5)

    # Step 3: Send test notes - C major scale
    print(f"\n[3] Sending C major scale (ch 1)...")
    notes = [60, 62, 64, 65, 67, 69, 71, 72]  # C D E F G A B C
    for note in notes:
        send_note(data_sock, addr, note, 100, channel=0)
        time.sleep(0.3)
        send_note(data_sock, addr, note, 0, channel=0)
        time.sleep(0.1)

    print(f"\n[4] Done. Checking daemon log...")
    time.sleep(1)

    # Step 5: Send BYE
    print(f"\n[5] Sending BYE...")
    bye = struct.pack('!HHIII', APPLEMIDI_SIG, CMD_BY, 2, TOKEN, LOCAL_SSRC)
    control_sock.sendto(bye, (addr, CONTROL_PORT))

    control_sock.close()
    data_sock.close()
    print("Done.")
    return 0


if __name__ == '__main__':
    sys.exit(main())
