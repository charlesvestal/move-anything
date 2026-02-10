/*
 * rtpmidi_daemon.c - RTP-MIDI daemon for Move Anything
 *
 * Receives wireless MIDI via AppleMIDI (RFC 6295) and writes
 * USB-MIDI packets into shared memory for the shim to merge
 * into the hardware mailbox.
 *
 * Usage: rtpmidi-daemon [--name <service-name>]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <dbus/dbus.h>

#include "../host/shadow_constants.h"
#include "rtpmidi.h"

/* ============================================================================
 * Globals
 * ============================================================================ */

static volatile int running = 1;
static shadow_rtp_midi_t *rtp_shm = NULL;
static rtpmidi_session_t session;
static const char *service_name = "Move";

/* ============================================================================
 * Signal handler
 * ============================================================================ */

static void signal_handler(int sig) {
    (void)sig;
    running = 0;
}

/* ============================================================================
 * Shared memory
 * ============================================================================ */

static int shm_init(void) {
    int fd = shm_open(SHM_SHADOW_RTP_MIDI, O_CREAT | O_RDWR, 0666);
    if (fd < 0) {
        perror("rtpmidi: shm_open");
        return -1;
    }

    if (ftruncate(fd, sizeof(shadow_rtp_midi_t)) < 0) {
        perror("rtpmidi: ftruncate");
        close(fd);
        return -1;
    }

    rtp_shm = (shadow_rtp_midi_t *)mmap(NULL, sizeof(shadow_rtp_midi_t),
                                         PROT_READ | PROT_WRITE,
                                         MAP_SHARED, fd, 0);
    close(fd);

    if (rtp_shm == MAP_FAILED) {
        perror("rtpmidi: mmap");
        rtp_shm = NULL;
        return -1;
    }

    /* Zero the buffer on startup */
    memset((void *)rtp_shm, 0, sizeof(shadow_rtp_midi_t));
    return 0;
}

/*
 * Write a 3-byte MIDI message into the shared memory buffer
 * as a USB-MIDI packet (4 bytes, cable 2).
 * Does NOT toggle the ready flag - call shm_flush() after writing
 * all messages for a given RTP packet to signal the shim once.
 */
static void shm_write_midi(uint8_t status, uint8_t d1, uint8_t d2) {
    if (!rtp_shm) return;

    uint16_t idx = rtp_shm->write_idx;
    if (idx + 4 > SHADOW_RTP_MIDI_BUFFER_SIZE) return;  /* Buffer full, drop */

    format_usb_midi_packet(&rtp_shm->buffer[idx], status, d1, d2);
    rtp_shm->write_idx = idx + 4;
}

/* Signal the shim that new MIDI data is available.
 * Call once after writing all messages from an RTP packet. */
static void shm_flush(void) {
    if (!rtp_shm) return;
    if (rtp_shm->write_idx == 0) return;  /* Nothing written */
    rtp_shm->ready++;  /* Increment (not toggle) so every flush is visible */
}

/* ============================================================================
 * UDP socket creation
 * ============================================================================ */

static int create_udp_socket(uint16_t port) {
    /* Use IPv6 dual-stack socket to accept both IPv4 and IPv6 connections.
     * macOS prefers IPv6 when resolving move.local, so we must handle both. */
    int fd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("rtpmidi: socket");
        return -1;
    }

    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("rtpmidi: setsockopt SO_REUSEADDR");
        close(fd);
        return -1;
    }

    /* Allow both IPv4 and IPv6 on the same socket */
    int v6only = 0;
    if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only)) < 0) {
        perror("rtpmidi: setsockopt IPV6_V6ONLY");
        close(fd);
        return -1;
    }

    struct sockaddr_in6 addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_any;
    addr.sin6_port = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "rtpmidi: bind port %u: %s\n", port, strerror(errno));
        close(fd);
        return -1;
    }

    printf("rtpmidi: listening on UDP port %u (dual-stack)\n", port);
    return fd;
}

/* ============================================================================
 * Address formatting helper (supports both IPv4 and IPv6)
 * ============================================================================ */

static const char *format_addr(struct sockaddr_storage *addr, char *buf, size_t len) {
    if (addr->ss_family == AF_INET) {
        struct sockaddr_in *v4 = (struct sockaddr_in *)addr;
        inet_ntop(AF_INET, &v4->sin_addr, buf, len);
    } else if (addr->ss_family == AF_INET6) {
        struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)addr;
        inet_ntop(AF_INET6, &v6->sin6_addr, buf, len);
    } else {
        snprintf(buf, len, "unknown");
    }
    return buf;
}

/* ============================================================================
 * AppleMIDI session management (control port)
 * ============================================================================ */

static void handle_control_packet(int sock_fd, const uint8_t *buf, int len,
                                  struct sockaddr_storage *src, socklen_t src_len) {
    if (len < 4) return;

    uint16_t sig, cmd;
    memcpy(&sig, buf + 0, 2);
    memcpy(&cmd, buf + 2, 2);
    sig = ntohs(sig);
    cmd = ntohs(cmd);

    if (sig != APPLEMIDI_SIGNATURE) return;

    if (cmd == APPLEMIDI_CMD_IN) {
        /* Invitation: parse version, token, SSRC, optional name */
        if (len < 16) return;

        uint32_t version, token, remote_ssrc;
        memcpy(&version, buf + 4, 4);
        memcpy(&token, buf + 8, 4);
        memcpy(&remote_ssrc, buf + 12, 4);
        version = ntohl(version);
        token = ntohl(token);
        remote_ssrc = ntohl(remote_ssrc);

        char peer_name[64] = "unknown";
        if (len > 16) {
            int name_len = len - 16;
            if (name_len > (int)sizeof(peer_name) - 1)
                name_len = (int)sizeof(peer_name) - 1;
            memcpy(peer_name, buf + 16, name_len);
            peer_name[name_len] = '\0';
        }

        char addr_buf[INET6_ADDRSTRLEN];
        printf("rtpmidi: IN from %s (SSRC=0x%08X name=%s)\n",
               format_addr(src, addr_buf, sizeof(addr_buf)), remote_ssrc, peer_name);

        /* Store session info */
        session.remote_ssrc = remote_ssrc;
        session.initiator_token = token;
        memcpy(&session.remote_addr, src, src_len);
        session.remote_addr_len = src_len;

        /* Build OK response */
        int name_len = (int)strlen(service_name) + 1;  /* include null */
        int reply_len = 16 + name_len;
        uint8_t reply[128];

        uint16_t r_sig = htons(APPLEMIDI_SIGNATURE);
        uint16_t r_cmd = htons(APPLEMIDI_CMD_OK);
        uint32_t r_version = htonl(2);
        uint32_t r_token = htonl(token);
        uint32_t r_ssrc = htonl(session.local_ssrc);

        memcpy(reply + 0, &r_sig, 2);
        memcpy(reply + 2, &r_cmd, 2);
        memcpy(reply + 4, &r_version, 4);
        memcpy(reply + 8, &r_token, 4);
        memcpy(reply + 12, &r_ssrc, 4);
        memcpy(reply + 16, service_name, name_len);

        sendto(sock_fd, reply, reply_len, 0,
               (struct sockaddr *)src, src_len);

        session.state = SESSION_CONNECTED;
        printf("rtpmidi: session CONNECTED with %s\n", peer_name);
    }
    else if (cmd == APPLEMIDI_CMD_CK) {
        /* Clock synchronization */
        if (len < 36) return;

        uint8_t count = buf[8];

        if (count == 0) {
            /* Respond with count=1 and our timestamp */
            uint8_t reply[36];
            memcpy(reply, buf, 36);

            reply[8] = 1;  /* count = 1 */

            /* Write our SSRC */
            uint32_t our_ssrc = htonl(session.local_ssrc);
            memcpy(reply + 4, &our_ssrc, 4);

            /* Timestamp2 at offset 20: our receive time */
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            uint64_t now = (uint64_t)ts.tv_sec * 10000 + (uint64_t)ts.tv_nsec / 100000;
            uint64_t net_ts = 0;
            /* Manual big-endian conversion for uint64 */
            for (int i = 0; i < 8; i++) {
                ((uint8_t *)&net_ts)[i] = (uint8_t)(now >> (56 - i * 8));
            }
            memcpy(reply + 20, &net_ts, 8);

            sendto(sock_fd, reply, 36, 0,
                   (struct sockaddr *)src, src_len);
        }
        /* count == 2: sync complete, nothing to do */
    }
    else if (cmd == APPLEMIDI_CMD_BY) {
        /* Bye */
        if (len < 16) return;

        uint32_t remote_ssrc;
        memcpy(&remote_ssrc, buf + 12, 4);
        remote_ssrc = ntohl(remote_ssrc);

        if (remote_ssrc == session.remote_ssrc) {
            char bye_addr[INET6_ADDRSTRLEN];
            printf("rtpmidi: BYE from %s (SSRC=0x%08X)\n",
                   format_addr(src, bye_addr, sizeof(bye_addr)), remote_ssrc);
            session.state = SESSION_IDLE;
        }
    }
}

/* ============================================================================
 * RTP-MIDI data packet parsing
 * ============================================================================ */

static void handle_data_packet(int sock_fd, const uint8_t *buf, int len,
                               struct sockaddr_storage *src, socklen_t src_len) {
    if (len < 4) return;

    /* AppleMIDI command packets can arrive on the data port too.
     * macOS sends a second invitation on the data port after the control
     * port handshake; we must accept it for the session to be established. */
    uint16_t sig;
    memcpy(&sig, buf, 2);
    if (ntohs(sig) == APPLEMIDI_SIGNATURE) {
        handle_control_packet(sock_fd, buf, len, src, src_len);
        return;
    }

    if (len < 13) return;

    /* Validate RTP header: version must be 2, payload type must be 0x61 */
    uint8_t rtp_version = (buf[0] >> 6) & 0x03;
    if (rtp_version != RTP_VERSION) return;

    uint8_t payload_type = buf[1] & 0x7F;
    if (payload_type != RTP_PAYLOAD_TYPE) return;

    /* Parse MIDI command section at offset 12 */
    int offset = 12;
    if (offset >= len) return;

    uint8_t flags = buf[offset];
    int b_flag = (flags >> 7) & 1;  /* Long header */
    int j_flag = (flags >> 6) & 1;  /* Journal present */
    int z_flag = (flags >> 5) & 1;  /* Zero delta for first message */
    /* int p_flag = (flags >> 4) & 1; */  /* Phantom - unused */

    int midi_list_len;
    if (b_flag) {
        /* Long header: 12-bit length */
        if (offset + 1 >= len) return;
        midi_list_len = ((flags & 0x0F) << 8) | buf[offset + 1];
        offset += 2;
    } else {
        /* Short header: 4-bit length */
        midi_list_len = flags & 0x0F;
        offset += 1;
    }

    if (midi_list_len == 0) return;

    int midi_end = offset + midi_list_len;
    if (midi_end > len) midi_end = len;

    /* Parse MIDI messages from the MIDI command section.
     *
     * RTP-MIDI format: cmd [delta cmd] [delta cmd] ...
     * - The first message has NO delta prefix in the byte stream.
     *   (Z flag indicates whether its timestamp is zero, but the delta
     *   value is not encoded in the stream for the first command.)
     * - Subsequent messages have a variable-length delta prefix.
     * - Delta encoding: continuation bytes have bit 7 set (0x80-0xFF),
     *   final byte has bit 7 clear (0x00-0x7F). Same as standard MIDI VLQ.
     * - Running status applies: if no new status byte, reuse previous. */
    uint8_t running_status = 0;
    int first_message = 1;

    while (offset < midi_end) {
        /* Skip delta-time between messages (only after the first).
         * The first message never has a delta prefix in the stream. */
        if (!first_message) {
            while (offset < midi_end && (buf[offset] & 0x80))
                offset++;           /* continuation bytes */
            if (offset < midi_end)
                offset++;           /* final byte of delta */
        }
        first_message = 0;

        if (offset >= midi_end) break;

        /* Read status byte or use running status */
        uint8_t byte = buf[offset];
        uint8_t status;

        if (byte >= 0xF8) {
            /* System realtime (0xF8-0xFF): single byte, no data */
            shm_write_midi(byte, 0, 0);
            offset++;
            continue;
        } else if (byte >= 0x80) {
            status = byte;
            running_status = status;
            offset++;
        } else {
            /* Running status */
            status = running_status;
            if (running_status == 0) {
                offset++;
                continue;  /* No running status to use, skip */
            }
        }

        if (offset >= midi_end) break;

        uint8_t high = status & 0xF0;

        if (high >= 0x80 && high <= 0xBF) {
            /* Note Off/On, Poly Pressure, CC: 2 data bytes */
            if (offset + 1 >= midi_end) break;
            shm_write_midi(status, buf[offset], buf[offset + 1]);
            offset += 2;
        }
        else if (high == 0xC0 || high == 0xD0) {
            /* Program Change, Channel Pressure: 1 data byte */
            shm_write_midi(status, buf[offset], 0);
            offset += 1;
        }
        else if (high == 0xE0) {
            /* Pitch Bend: 2 data bytes */
            if (offset + 1 >= midi_end) break;
            shm_write_midi(status, buf[offset], buf[offset + 1]);
            offset += 2;
        }
        else if (status == 0xF0) {
            /* SysEx: skip until 0xF7 */
            while (offset < midi_end && buf[offset] != 0xF7) offset++;
            if (offset < midi_end) offset++;
        }
        else {
            offset++;  /* Skip unknown */
        }
    }

    (void)j_flag;  /* Journal parsing intentionally not implemented */

    /* Signal the shim once after all messages in this packet are written */
    shm_flush();
}

/* ============================================================================
 * Avahi service advertisement via D-Bus
 *
 * Registers _apple-midi._udp with the system Avahi daemon using the D-Bus API.
 * The registration lives as long as the D-Bus connection, so when the daemon
 * exits the service is automatically removed. No files on the root partition.
 * ============================================================================ */

static DBusConnection *avahi_bus = NULL;
static char avahi_group_path[256] = "";

static void avahi_register(const char *name, uint16_t port) {
    DBusError err;
    dbus_error_init(&err);

    avahi_bus = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
    if (dbus_error_is_set(&err) || !avahi_bus) {
        fprintf(stderr, "rtpmidi: D-Bus connect failed: %s\n",
                err.message ? err.message : "unknown");
        dbus_error_free(&err);
        return;
    }

    /* Step 1: Create an EntryGroup */
    DBusMessage *msg = dbus_message_new_method_call(
        "org.freedesktop.Avahi", "/",
        "org.freedesktop.Avahi.Server", "EntryGroupNew");
    if (!msg) goto fail;

    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        avahi_bus, msg, 2000, &err);
    dbus_message_unref(msg);

    if (dbus_error_is_set(&err) || !reply) {
        fprintf(stderr, "rtpmidi: EntryGroupNew failed: %s\n",
                err.message ? err.message : "no reply");
        dbus_error_free(&err);
        goto fail;
    }

    const char *group_path_ptr = NULL;
    if (!dbus_message_get_args(reply, &err,
            DBUS_TYPE_OBJECT_PATH, &group_path_ptr, DBUS_TYPE_INVALID)) {
        fprintf(stderr, "rtpmidi: EntryGroupNew parse failed: %s\n", err.message);
        dbus_message_unref(reply);
        dbus_error_free(&err);
        goto fail;
    }
    snprintf(avahi_group_path, sizeof(avahi_group_path), "%s", group_path_ptr);
    dbus_message_unref(reply);

    /* Step 2: AddService(_apple-midi._udp, port) */
    msg = dbus_message_new_method_call(
        "org.freedesktop.Avahi", avahi_group_path,
        "org.freedesktop.Avahi.EntryGroup", "AddService");
    if (!msg) goto fail;

    dbus_int32_t iface = -1;   /* AVAHI_IF_UNSPEC */
    dbus_int32_t proto = -1;   /* AVAHI_PROTO_UNSPEC (dual-stack socket handles both) */
    dbus_uint32_t flags = 0;
    const char *svc_type = "_apple-midi._udp";
    const char *domain = "";
    const char *host = "";
    dbus_uint16_t dbus_port = port;

    dbus_message_append_args(msg,
        DBUS_TYPE_INT32, &iface,
        DBUS_TYPE_INT32, &proto,
        DBUS_TYPE_UINT32, &flags,
        DBUS_TYPE_STRING, &name,
        DBUS_TYPE_STRING, &svc_type,
        DBUS_TYPE_STRING, &domain,
        DBUS_TYPE_STRING, &host,
        DBUS_TYPE_UINT16, &dbus_port,
        DBUS_TYPE_INVALID);

    /* Empty TXT record array */
    DBusMessageIter iter, sub;
    dbus_message_iter_init_append(msg, &iter);
    /* We already appended basic args, need to add the array at the end.
     * Re-do: build message manually for the array. */
    dbus_message_unref(msg);

    msg = dbus_message_new_method_call(
        "org.freedesktop.Avahi", avahi_group_path,
        "org.freedesktop.Avahi.EntryGroup", "AddService");

    dbus_message_iter_init_append(msg, &iter);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_INT32, &iface);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_INT32, &proto);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_UINT32, &flags);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &name);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &svc_type);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &domain);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &host);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_UINT16, &dbus_port);

    /* TXT records: array of array of bytes (aay) - empty */
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "ay", &sub);
    dbus_message_iter_close_container(&iter, &sub);

    reply = dbus_connection_send_with_reply_and_block(avahi_bus, msg, 2000, &err);
    dbus_message_unref(msg);

    if (dbus_error_is_set(&err)) {
        fprintf(stderr, "rtpmidi: AddService failed: %s\n", err.message);
        dbus_error_free(&err);
        goto fail;
    }
    if (reply) dbus_message_unref(reply);

    /* Step 3: Commit */
    msg = dbus_message_new_method_call(
        "org.freedesktop.Avahi", avahi_group_path,
        "org.freedesktop.Avahi.EntryGroup", "Commit");

    reply = dbus_connection_send_with_reply_and_block(avahi_bus, msg, 2000, &err);
    dbus_message_unref(msg);

    if (dbus_error_is_set(&err)) {
        fprintf(stderr, "rtpmidi: Commit failed: %s\n", err.message);
        dbus_error_free(&err);
        goto fail;
    }
    if (reply) dbus_message_unref(reply);

    printf("rtpmidi: registered Avahi service '%s' on port %u\n", name, port);
    return;

fail:
    fprintf(stderr, "rtpmidi: Avahi registration failed (service won't be discoverable)\n");
    if (avahi_bus) {
        dbus_connection_unref(avahi_bus);
        avahi_bus = NULL;
    }
}

static void avahi_unregister(void) {
    if (!avahi_bus) return;

    if (avahi_group_path[0]) {
        DBusError err;
        dbus_error_init(&err);
        DBusMessage *msg = dbus_message_new_method_call(
            "org.freedesktop.Avahi", avahi_group_path,
            "org.freedesktop.Avahi.EntryGroup", "Free");
        if (msg) {
            DBusMessage *reply = dbus_connection_send_with_reply_and_block(
                avahi_bus, msg, 2000, &err);
            dbus_message_unref(msg);
            if (reply) dbus_message_unref(reply);
            dbus_error_free(&err);
        }
    }

    dbus_connection_unref(avahi_bus);
    avahi_bus = NULL;
    printf("rtpmidi: unregistered Avahi service\n");
}

/* ============================================================================
 * Send BYE to connected peer
 * ============================================================================ */

static void send_bye(int control_fd) {
    if (session.state != SESSION_CONNECTED) return;

    uint8_t pkt[16];
    uint16_t sig = htons(APPLEMIDI_SIGNATURE);
    uint16_t cmd = htons(APPLEMIDI_CMD_BY);

    memcpy(pkt + 0, &sig, 2);
    memcpy(pkt + 2, &cmd, 2);

    uint32_t version = htonl(2);
    memcpy(pkt + 4, &version, 4);

    uint32_t token = htonl(session.initiator_token);
    memcpy(pkt + 8, &token, 4);

    uint32_t ssrc = htonl(session.local_ssrc);
    memcpy(pkt + 12, &ssrc, 4);

    sendto(control_fd, pkt, sizeof(pkt), 0,
           (struct sockaddr *)&session.remote_addr, session.remote_addr_len);

    char bye_addr[INET6_ADDRSTRLEN];
    printf("rtpmidi: sent BYE to %s\n",
           format_addr(&session.remote_addr, bye_addr, sizeof(bye_addr)));

    session.state = SESSION_IDLE;
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(int argc, char *argv[]) {
    /* Line-buffer stdout so logs appear immediately when redirected */
    setlinebuf(stdout);

    /* Parse optional --name argument */
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "--name") == 0) {
            service_name = argv[++i];
        }
    }

    printf("rtpmidi: starting RTP-MIDI daemon (%s)\n", service_name);

    /* Set up signal handlers */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Initialize shared memory */
    if (shm_init() < 0) {
        fprintf(stderr, "rtpmidi: failed to init shared memory\n");
        return 1;
    }

    /* Create UDP sockets */
    int control_fd = create_udp_socket(RTPMIDI_CONTROL_PORT);
    if (control_fd < 0) {
        fprintf(stderr, "rtpmidi: failed to create control socket\n");
        return 1;
    }

    int data_fd = create_udp_socket(RTPMIDI_DATA_PORT);
    if (data_fd < 0) {
        fprintf(stderr, "rtpmidi: failed to create data socket\n");
        close(control_fd);
        return 1;
    }

    /* Register with Avahi for mDNS advertisement */
    avahi_register(service_name, RTPMIDI_CONTROL_PORT);

    /* Initialize session */
    memset(&session, 0, sizeof(session));
    session.local_ssrc = (uint32_t)getpid();
    session.state = SESSION_IDLE;

    printf("rtpmidi: daemon ready (SSRC=0x%08X)\n", session.local_ssrc);

    /* Main poll loop */
    struct pollfd fds[2];
    fds[0].fd = control_fd;
    fds[0].events = POLLIN;
    fds[1].fd = data_fd;
    fds[1].events = POLLIN;

    uint8_t buf[2048];

    while (running) {
        int ret = poll(fds, 2, 1000);

        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("rtpmidi: poll");
            break;
        }

        if (ret == 0) continue;  /* Timeout - nothing to do */

        /* Control port */
        if (fds[0].revents & POLLIN) {
            struct sockaddr_storage src;
            socklen_t src_len = sizeof(src);
            int n = recvfrom(control_fd, buf, sizeof(buf), 0,
                             (struct sockaddr *)&src, &src_len);
            if (n > 0) {
                handle_control_packet(control_fd, buf, n, &src, src_len);
            }
        }

        /* Data port */
        if (fds[1].revents & POLLIN) {
            struct sockaddr_storage src;
            socklen_t src_len = sizeof(src);
            int n = recvfrom(data_fd, buf, sizeof(buf), 0,
                             (struct sockaddr *)&src, &src_len);
            if (n > 0) {
                handle_data_packet(data_fd, buf, n, &src, src_len);
            }
        }
    }

    /* Shutdown */
    printf("rtpmidi: shutting down\n");

    send_bye(control_fd);
    avahi_unregister();

    close(control_fd);
    close(data_fd);

    if (rtp_shm) {
        munmap(rtp_shm, sizeof(shadow_rtp_midi_t));
    }

    printf("rtpmidi: stopped\n");
    return 0;
}
