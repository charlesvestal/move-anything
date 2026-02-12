#!/usr/bin/env python3
"""
RTP-MIDI daemon for Move Anything (pure Python, no dependencies).

Receives wireless MIDI via AppleMIDI (RFC 6295) and writes USB-MIDI
packets into shared memory for the shim to merge into the hardware
mailbox.

Usage: python3 rtpmidi_server.py [--name <service-name>] [--port <port>]
"""

import argparse
import ctypes
import ctypes.util
import mmap
import os
import select
import signal
import socket
import struct
import sys
import time

# --- Constants ---

APPLEMIDI_SIGNATURE = 0xFFFF
APPLEMIDI_CMD_IN = 0x494E   # Invitation
APPLEMIDI_CMD_OK = 0x4F4B   # Accept
APPLEMIDI_CMD_NO = 0x4E4F   # Reject
APPLEMIDI_CMD_BY = 0x4259   # Bye
APPLEMIDI_CMD_CK = 0x434B   # Clock sync

RTP_VERSION = 2
RTP_PAYLOAD_TYPE = 0x61     # 97 - standard for RTP-MIDI

CONTROL_PORT = 5004
DATA_PORT = 5005

SHM_NAME = "/move-shadow-rtp-midi"
SHM_BUFFER_SIZE = 256       # Must match SHADOW_RTP_MIDI_BUFFER_SIZE

# shadow_rtp_midi_t layout:
#   uint16_t write_idx  (offset 0)
#   uint8_t  ready      (offset 2)
#   uint8_t  reserved   (offset 3)
#   uint8_t  buffer[256](offset 4)
SHM_TOTAL_SIZE = 4 + SHM_BUFFER_SIZE
SHM_WRITE_IDX_OFF = 0
SHM_READY_OFF = 2
SHM_BUFFER_OFF = 4

# --- Globals ---

running = True
service_name = "Move"


def signal_handler(signum, frame):
    global running
    running = False


# --- Shared Memory ---

class ShmWriter:
    """Write USB-MIDI packets to shared memory for the shim."""

    def __init__(self):
        self.fd = None
        self.mm = None

    def open(self):
        # Use ctypes to call shm_open (not in Python stdlib)
        rt = ctypes.CDLL("librt.so.1", use_errno=True)
        rt.shm_open.restype = ctypes.c_int
        rt.shm_open.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_uint]

        O_CREAT = 0o100
        O_RDWR = 0o2
        self.fd = rt.shm_open(SHM_NAME.encode(), O_CREAT | O_RDWR, 0o666)
        if self.fd < 0:
            print(f"rtpmidi: shm_open failed (errno {ctypes.get_errno()})")
            return False

        os.ftruncate(self.fd, SHM_TOTAL_SIZE)
        self.mm = mmap.mmap(self.fd, SHM_TOTAL_SIZE)

        # Zero on startup
        self.mm[:SHM_TOTAL_SIZE] = b'\x00' * SHM_TOTAL_SIZE
        return True

    def write_midi(self, status, d1, d2):
        """Write a 3-byte MIDI message as a USB-MIDI packet (cable 2)."""
        if not self.mm:
            return

        idx = struct.unpack_from('<H', self.mm, SHM_WRITE_IDX_OFF)[0]
        if idx + 4 > SHM_BUFFER_SIZE:
            print(f"rtpmidi: shm buffer full, dropping {status:02x} {d1:02x} {d2:02x}")
            return  # Buffer full, drop

        cin = (status >> 4) & 0x0F
        packet = bytes([0x20 | cin, status, d1, d2])  # Cable 2 (external MIDI)
        self.mm[SHM_BUFFER_OFF + idx:SHM_BUFFER_OFF + idx + 4] = packet
        struct.pack_into('<H', self.mm, SHM_WRITE_IDX_OFF, idx + 4)
        print(f"rtpmidi: MIDI [{cin:x}] {status:02x} {d1:02x} {d2:02x} -> shm idx={idx}")

    def flush(self):
        """Signal the shim that new data is available."""
        if not self.mm:
            return
        idx = struct.unpack_from('<H', self.mm, SHM_WRITE_IDX_OFF)[0]
        if idx == 0:
            return
        ready = self.mm[SHM_READY_OFF]
        new_ready = (ready + 1) & 0xFF
        self.mm[SHM_READY_OFF] = new_ready
        print(f"rtpmidi: flush idx={idx} ready {ready}->{new_ready}")

    def close(self):
        if self.mm:
            self.mm.close()
        if self.fd is not None and self.fd >= 0:
            os.close(self.fd)


# --- Avahi D-Bus Registration (via ctypes) ---

class AvahiRegistration:
    """Register _apple-midi._udp with Avahi via D-Bus (libdbus-1)."""

    def __init__(self):
        self.lib = None
        self.conn = None
        self.group_path = None

    def register(self, name, port):
        try:
            self.lib = ctypes.CDLL("libdbus-1.so.3", use_errno=True)
        except OSError:
            print("rtpmidi: libdbus-1.so.3 not found, skipping Avahi")
            return False

        # Set up function signatures
        lib = self.lib
        lib.dbus_error_init.argtypes = [ctypes.c_void_p]
        lib.dbus_error_init.restype = None
        lib.dbus_error_is_set.argtypes = [ctypes.c_void_p]
        lib.dbus_error_is_set.restype = ctypes.c_int
        lib.dbus_error_free.argtypes = [ctypes.c_void_p]
        lib.dbus_error_free.restype = None

        # DBusError is a struct: const char *name, const char *message, ...
        # Allocate enough space (typically 24 bytes on 64-bit)
        err = ctypes.create_string_buffer(64)
        lib.dbus_error_init(err)

        lib.dbus_bus_get.argtypes = [ctypes.c_int, ctypes.c_void_p]
        lib.dbus_bus_get.restype = ctypes.c_void_p
        DBUS_BUS_SYSTEM = 1  # 0=SESSION, 1=SYSTEM

        self.conn = lib.dbus_bus_get(DBUS_BUS_SYSTEM, err)
        if not self.conn or lib.dbus_error_is_set(err):
            print("rtpmidi: D-Bus connect failed")
            lib.dbus_error_free(err)
            return False

        # Helper for method calls
        def call_method(dest, path, iface, method, args_fn=None):
            lib.dbus_message_new_method_call.argtypes = [
                ctypes.c_char_p, ctypes.c_char_p,
                ctypes.c_char_p, ctypes.c_char_p
            ]
            lib.dbus_message_new_method_call.restype = ctypes.c_void_p
            msg = lib.dbus_message_new_method_call(
                dest.encode(), path.encode(),
                iface.encode(), method.encode()
            )
            if not msg:
                return None

            if args_fn:
                args_fn(msg)

            lib.dbus_connection_send_with_reply_and_block.argtypes = [
                ctypes.c_void_p, ctypes.c_void_p,
                ctypes.c_int, ctypes.c_void_p
            ]
            lib.dbus_connection_send_with_reply_and_block.restype = ctypes.c_void_p
            reply = lib.dbus_connection_send_with_reply_and_block(
                self.conn, msg, 2000, err
            )

            lib.dbus_message_unref.argtypes = [ctypes.c_void_p]
            lib.dbus_message_unref(msg)

            if lib.dbus_error_is_set(err):
                lib.dbus_error_free(err)
                return None
            return reply

        # Step 1: EntryGroupNew
        reply = call_method(
            "org.freedesktop.Avahi", "/",
            "org.freedesktop.Avahi.Server", "EntryGroupNew"
        )
        if not reply:
            print("rtpmidi: Avahi EntryGroupNew failed")
            return False

        # Extract object path from reply
        lib.dbus_message_get_args.argtypes = [
            ctypes.c_void_p, ctypes.c_void_p,
            ctypes.c_int, ctypes.POINTER(ctypes.c_char_p),
            ctypes.c_int
        ]
        lib.dbus_message_get_args.restype = ctypes.c_int

        DBUS_TYPE_OBJECT_PATH = ord('o')
        DBUS_TYPE_INVALID = 0

        path_ptr = ctypes.c_char_p()
        ok = lib.dbus_message_get_args(
            reply, err,
            DBUS_TYPE_OBJECT_PATH, ctypes.byref(path_ptr),
            DBUS_TYPE_INVALID
        )
        if not ok:
            lib.dbus_message_unref(reply)
            print("rtpmidi: Avahi EntryGroupNew parse failed")
            return False

        self.group_path = path_ptr.value.decode()
        lib.dbus_message_unref(reply)

        # Step 2: AddService
        def add_service_args(msg):
            lib.dbus_message_append_args.argtypes = [ctypes.c_void_p]
            lib.dbus_message_append_args.restype = ctypes.c_int

            # Build message with iterator for the TXT array
            iter_buf = ctypes.create_string_buffer(256)
            sub_buf = ctypes.create_string_buffer(256)

            lib.dbus_message_iter_init_append.argtypes = [
                ctypes.c_void_p, ctypes.c_void_p
            ]
            lib.dbus_message_iter_init_append(msg, iter_buf)

            DBUS_TYPE_INT32 = ord('i')
            DBUS_TYPE_UINT32 = ord('u')
            DBUS_TYPE_STRING = ord('s')
            DBUS_TYPE_UINT16 = ord('q')
            DBUS_TYPE_ARRAY = ord('a')

            lib.dbus_message_iter_append_basic.argtypes = [
                ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p
            ]

            iface_val = ctypes.c_int32(-1)   # AVAHI_IF_UNSPEC
            proto_val = ctypes.c_int32(-1)    # AVAHI_PROTO_UNSPEC
            flags_val = ctypes.c_uint32(0)
            name_val = ctypes.c_char_p(name.encode())
            type_val = ctypes.c_char_p(b"_apple-midi._udp")
            domain_val = ctypes.c_char_p(b"")
            host_val = ctypes.c_char_p(b"")
            port_val = ctypes.c_uint16(port)

            lib.dbus_message_iter_append_basic(iter_buf, DBUS_TYPE_INT32, ctypes.byref(iface_val))
            lib.dbus_message_iter_append_basic(iter_buf, DBUS_TYPE_INT32, ctypes.byref(proto_val))
            lib.dbus_message_iter_append_basic(iter_buf, DBUS_TYPE_UINT32, ctypes.byref(flags_val))
            lib.dbus_message_iter_append_basic(iter_buf, DBUS_TYPE_STRING, ctypes.byref(name_val))
            lib.dbus_message_iter_append_basic(iter_buf, DBUS_TYPE_STRING, ctypes.byref(type_val))
            lib.dbus_message_iter_append_basic(iter_buf, DBUS_TYPE_STRING, ctypes.byref(domain_val))
            lib.dbus_message_iter_append_basic(iter_buf, DBUS_TYPE_STRING, ctypes.byref(host_val))
            lib.dbus_message_iter_append_basic(iter_buf, DBUS_TYPE_UINT16, ctypes.byref(port_val))

            # Empty TXT record array (aay)
            lib.dbus_message_iter_open_container.argtypes = [
                ctypes.c_void_p, ctypes.c_int,
                ctypes.c_char_p, ctypes.c_void_p
            ]
            lib.dbus_message_iter_close_container.argtypes = [
                ctypes.c_void_p, ctypes.c_void_p
            ]
            lib.dbus_message_iter_open_container(
                iter_buf, DBUS_TYPE_ARRAY, b"ay", sub_buf
            )
            lib.dbus_message_iter_close_container(iter_buf, sub_buf)

        reply = call_method(
            "org.freedesktop.Avahi", self.group_path,
            "org.freedesktop.Avahi.EntryGroup", "AddService",
            add_service_args
        )
        if not reply:
            print("rtpmidi: Avahi AddService failed")
            return False
        lib.dbus_message_unref(reply)

        # Step 3: Commit
        reply = call_method(
            "org.freedesktop.Avahi", self.group_path,
            "org.freedesktop.Avahi.EntryGroup", "Commit"
        )
        if not reply:
            print("rtpmidi: Avahi Commit failed")
            return False
        lib.dbus_message_unref(reply)

        print(f"rtpmidi: registered Avahi service '{name}' on port {port}")
        return True

    def unregister(self):
        if not self.lib or not self.conn or not self.group_path:
            return
        try:
            err = ctypes.create_string_buffer(64)
            self.lib.dbus_error_init(err)
            msg = self.lib.dbus_message_new_method_call(
                b"org.freedesktop.Avahi",
                self.group_path.encode(),
                b"org.freedesktop.Avahi.EntryGroup",
                b"Free"
            )
            if msg:
                reply = self.lib.dbus_connection_send_with_reply_and_block(
                    self.conn, msg, 2000, err
                )
                self.lib.dbus_message_unref(msg)
                if reply:
                    self.lib.dbus_message_unref(reply)
            self.lib.dbus_error_free(err)
            self.lib.dbus_connection_unref(self.conn)
        except Exception:
            pass
        print("rtpmidi: unregistered Avahi service")


# --- AppleMIDI Session ---

class Session:
    def __init__(self):
        self.state = 'idle'
        self.remote_ssrc = 0
        self.local_ssrc = os.getpid() & 0xFFFFFFFF
        self.initiator_token = 0
        self.remote_addr = None

    def handle_control(self, sock, data, addr):
        if len(data) < 4:
            return
        sig, cmd = struct.unpack('!HH', data[:4])
        if sig != APPLEMIDI_SIGNATURE:
            return

        if cmd == APPLEMIDI_CMD_IN:
            if len(data) < 16:
                return
            version, token, remote_ssrc = struct.unpack('!III', data[4:16])
            peer_name = data[16:].split(b'\x00')[0].decode('utf-8', errors='replace') if len(data) > 16 else 'unknown'

            self.remote_ssrc = remote_ssrc
            self.initiator_token = token
            self.remote_addr = addr

            # Send OK
            reply = struct.pack('!HHIII',
                APPLEMIDI_SIGNATURE, APPLEMIDI_CMD_OK,
                2, token, self.local_ssrc
            ) + service_name.encode() + b'\x00'
            sock.sendto(reply, addr)

            self.state = 'connected'
            print(f"rtpmidi: session CONNECTED with {peer_name} ({addr[0]})")

        elif cmd == APPLEMIDI_CMD_CK:
            if len(data) < 36:
                return
            count = data[8]
            if count == 0:
                reply = bytearray(data[:36])
                reply[8] = 1  # count = 1
                # Write our SSRC
                struct.pack_into('!I', reply, 4, self.local_ssrc)
                # Timestamp2: our receive time
                now = int(time.monotonic() * 10000)
                struct.pack_into('!Q', reply, 20, now)
                sock.sendto(bytes(reply), addr)

        elif cmd == APPLEMIDI_CMD_BY:
            if len(data) < 16:
                return
            remote_ssrc = struct.unpack('!I', data[12:16])[0]
            if remote_ssrc == self.remote_ssrc:
                print(f"rtpmidi: BYE from {addr[0]}")
                self.state = 'idle'

    def send_bye(self, sock):
        if self.state != 'connected' or not self.remote_addr:
            return
        pkt = struct.pack('!HHIII',
            APPLEMIDI_SIGNATURE, APPLEMIDI_CMD_BY,
            2, self.initiator_token, self.local_ssrc
        )
        try:
            sock.sendto(pkt, self.remote_addr)
        except OSError:
            pass
        print(f"rtpmidi: sent BYE")
        self.state = 'idle'


# --- RTP-MIDI Parser ---

def parse_midi_commands(data, shm):
    """Parse an RTP-MIDI packet and write MIDI messages to shm."""
    if len(data) < 13:
        return

    # Check for AppleMIDI command on data port
    sig = struct.unpack('!H', data[:2])[0]
    if sig == APPLEMIDI_SIGNATURE:
        return data  # Signal caller to handle as control packet

    # Validate RTP header
    rtp_version = (data[0] >> 6) & 0x03
    if rtp_version != RTP_VERSION:
        return
    payload_type = data[1] & 0x7F
    if payload_type != RTP_PAYLOAD_TYPE:
        return

    # MIDI command section at offset 12
    offset = 12
    if offset >= len(data):
        return

    flags = data[offset]
    b_flag = (flags >> 7) & 1
    z_flag = (flags >> 5) & 1

    if b_flag:
        if offset + 1 >= len(data):
            return
        midi_len = ((flags & 0x0F) << 8) | data[offset + 1]
        offset += 2
    else:
        midi_len = flags & 0x0F
        offset += 1

    if midi_len == 0:
        return

    midi_end = min(offset + midi_len, len(data))
    running_status = 0
    first_message = True

    while offset < midi_end:
        # Skip delta-time between messages (not for first message)
        if not first_message:
            while offset < midi_end and (data[offset] & 0x80):
                offset += 1  # continuation bytes
            if offset < midi_end:
                offset += 1  # final byte
        first_message = False

        if offset >= midi_end:
            break

        byte = data[offset]

        if byte >= 0xF8:
            # System realtime
            shm.write_midi(byte, 0, 0)
            offset += 1
            continue
        elif byte >= 0x80:
            status = byte
            running_status = status
            offset += 1
        else:
            # Running status
            status = running_status
            if running_status == 0:
                offset += 1
                continue

        if offset >= midi_end:
            break

        high = status & 0xF0

        if 0x80 <= high <= 0xBF:
            # Note Off/On, Poly Pressure, CC: 2 data bytes
            if offset + 1 >= midi_end:
                break
            shm.write_midi(status, data[offset], data[offset + 1])
            offset += 2
        elif high in (0xC0, 0xD0):
            # Program Change, Channel Pressure: 1 data byte
            shm.write_midi(status, data[offset], 0)
            offset += 1
        elif high == 0xE0:
            # Pitch Bend: 2 data bytes
            if offset + 1 >= midi_end:
                break
            shm.write_midi(status, data[offset], data[offset + 1])
            offset += 2
        elif status == 0xF0:
            # SysEx: skip to 0xF7
            while offset < midi_end and data[offset] != 0xF7:
                offset += 1
            if offset < midi_end:
                offset += 1
        else:
            offset += 1

    shm.flush()


# --- Main ---

def main():
    global service_name, running

    parser = argparse.ArgumentParser(description='RTP-MIDI daemon')
    parser.add_argument('--name', default='Move', help='Service name')
    parser.add_argument('--port', type=int, default=CONTROL_PORT, help='Control port')
    args = parser.parse_args()

    service_name = args.name
    control_port = args.port
    data_port = control_port + 1

    # Line-buffer stdout
    sys.stdout.reconfigure(line_buffering=True)

    # Kill any existing rtpmidi_server instances before starting
    my_pid = os.getpid()
    try:
        import subprocess
        result = subprocess.run(['pgrep', '-f', 'rtpmidi_server'],
                                capture_output=True, text=True, timeout=5)
        for line in result.stdout.strip().split('\n'):
            if line.strip():
                pid = int(line.strip())
                if pid != my_pid:
                    print(f"rtpmidi: killing existing instance pid={pid}")
                    os.kill(pid, signal.SIGTERM)
        time.sleep(0.3)
    except Exception:
        pass

    print(f"rtpmidi: starting RTP-MIDI daemon ({service_name})")

    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    # Initialize shared memory
    shm = ShmWriter()
    if not shm.open():
        print("rtpmidi: failed to init shared memory")
        return 1

    # Create dual-stack UDP sockets
    control_sock = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
    control_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    control_sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
    control_sock.bind(('::', control_port))
    print(f"rtpmidi: listening on UDP port {control_port} (dual-stack)")

    data_sock = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
    data_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    data_sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
    data_sock.bind(('::', data_port))
    print(f"rtpmidi: listening on UDP port {data_port} (dual-stack)")

    # Register with Avahi
    avahi = AvahiRegistration()
    avahi.register(service_name, control_port)

    # Session
    session = Session()
    print(f"rtpmidi: daemon ready (SSRC=0x{session.local_ssrc:08X})")

    # Main loop
    try:
        while running:
            readable, _, _ = select.select(
                [control_sock, data_sock], [], [], 1.0
            )

            for sock in readable:
                try:
                    data, addr = sock.recvfrom(2048)
                except OSError:
                    continue

                if sock is control_sock:
                    session.handle_control(control_sock, data, addr)
                else:
                    # Data port: could be AppleMIDI or RTP-MIDI
                    print(f"rtpmidi: data port recv {len(data)} bytes from {addr[0]}")
                    result = parse_midi_commands(data, shm)
                    if result is not None:
                        # Was an AppleMIDI command on data port
                        session.handle_control(data_sock, data, addr)
    except Exception as e:
        print(f"rtpmidi: error: {e}")

    # Shutdown
    print("rtpmidi: shutting down")
    session.send_bye(control_sock)
    avahi.unregister()
    control_sock.close()
    data_sock.close()
    shm.close()
    print("rtpmidi: stopped")
    return 0


if __name__ == '__main__':
    sys.exit(main())
