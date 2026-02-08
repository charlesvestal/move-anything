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
#include <net/if.h>
#include <ifaddrs.h>

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
 * Write a 3-byte MIDI message into the shared memory ring buffer
 * as a USB-MIDI packet (4 bytes, cable 2).
 */
static void shm_write_midi(uint8_t status, uint8_t d1, uint8_t d2) {
    if (!rtp_shm) return;

    uint8_t idx = rtp_shm->write_idx;
    uint8_t *slot = &rtp_shm->buffer[idx];

    format_usb_midi_packet(slot, status, d1, d2);

    /* Advance write index, wrapping within buffer (4 bytes per packet) */
    idx += 4;
    if (idx >= SHADOW_RTP_MIDI_BUFFER_SIZE)
        idx = 0;

    rtp_shm->write_idx = idx;
    rtp_shm->ready ^= 1;  /* Toggle to signal new data */
}

/* ============================================================================
 * UDP socket creation
 * ============================================================================ */

static int create_udp_socket(uint16_t port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
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

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "rtpmidi: bind port %u: %s\n", port, strerror(errno));
        close(fd);
        return -1;
    }

    printf("rtpmidi: listening on UDP port %u\n", port);
    return fd;
}

/* ============================================================================
 * AppleMIDI session management (control port)
 * ============================================================================ */

static void handle_control_packet(int sock_fd, const uint8_t *buf, int len,
                                  struct sockaddr_in *src, socklen_t src_len) {
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

        printf("rtpmidi: IN from %s (SSRC=0x%08X name=%s)\n",
               inet_ntoa(src->sin_addr), remote_ssrc, peer_name);

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
            printf("rtpmidi: BYE from %s (SSRC=0x%08X)\n",
                   inet_ntoa(src->sin_addr), remote_ssrc);
            session.state = SESSION_IDLE;
        }
    }
}

/* ============================================================================
 * RTP-MIDI data packet parsing
 * ============================================================================ */

static void handle_data_packet(int sock_fd, const uint8_t *buf, int len,
                               struct sockaddr_in *src, socklen_t src_len) {
    (void)sock_fd;
    (void)src;
    (void)src_len;

    if (len < 13) return;

    /* Validate RTP header: version must be 2, payload type must be 0x61 */
    uint8_t rtp_byte0 = buf[0];
    uint8_t rtp_version = (rtp_byte0 >> 6) & 0x03;
    if (rtp_version != RTP_VERSION) return;

    uint8_t rtp_byte1 = buf[1];
    uint8_t payload_type = rtp_byte1 & 0x7F;
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

    /* Parse MIDI messages from the list */
    uint8_t running_status = 0;
    int first_message = 1;

    while (offset < midi_end) {
        /* Skip delta-time (variable length) unless Z flag and first message */
        if (!first_message || !z_flag) {
            /* Variable-length delta: continuation bytes have bit 7 set */
            while (offset < midi_end && (buf[offset] & 0x80)) {
                offset++;
            }
            if (offset < midi_end) offset++;  /* Final byte of delta */
        }
        first_message = 0;

        if (offset >= midi_end) break;

        /* Read status or data byte */
        uint8_t byte = buf[offset];
        uint8_t status;

        if (byte & 0x80) {
            /* Status byte */
            status = byte;
            running_status = status;
            offset++;
        } else {
            /* Data byte - use running status */
            status = running_status;
            if (status == 0) {
                offset++;
                continue;  /* No running status established */
            }
        }

        if (offset > midi_end) break;

        /* Determine message length and dispatch */
        uint8_t high = status & 0xF0;

        if (high >= 0x80 && high <= 0xBF) {
            /* Note Off, Note On, Poly Pressure, Control Change: 2 data bytes */
            if (offset + 1 >= midi_end) break;
            uint8_t d1 = buf[offset];
            uint8_t d2 = buf[offset + 1];
            offset += 2;
            shm_write_midi(status, d1, d2);
        }
        else if (high == 0xC0 || high == 0xD0) {
            /* Program Change, Channel Pressure: 1 data byte */
            if (offset >= midi_end) break;
            uint8_t d1 = buf[offset];
            offset += 1;
            shm_write_midi(status, d1, 0);
        }
        else if (high == 0xE0) {
            /* Pitch Bend: 2 data bytes */
            if (offset + 1 >= midi_end) break;
            uint8_t d1 = buf[offset];
            uint8_t d2 = buf[offset + 1];
            offset += 2;
            shm_write_midi(status, d1, d2);
        }
        else if (status == 0xF0) {
            /* SysEx: skip until 0xF7 */
            while (offset < midi_end && buf[offset] != 0xF7) {
                offset++;
            }
            if (offset < midi_end) offset++;  /* Skip 0xF7 */
        }
        else if (status >= 0xF8) {
            /* System realtime: no data bytes */
            shm_write_midi(status, 0, 0);
        }
        else {
            /* Other system common (0xF1-0xF7): skip */
            break;
        }
    }

    (void)j_flag;  /* Journal parsing intentionally not implemented */
}

/* ============================================================================
 * mDNS service advertisement
 * ============================================================================ */

/* Get the local non-loopback IPv4 address */
static uint32_t get_local_ip(void) {
    struct ifaddrs *ifaddr, *ifa;
    uint32_t ip = 0;

    if (getifaddrs(&ifaddr) < 0) return 0;

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;
        if (ifa->ifa_flags & IFF_LOOPBACK) continue;

        struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
        ip = sa->sin_addr.s_addr;  /* Already in network byte order */
        break;
    }

    freeifaddrs(ifaddr);
    return ip;
}

/* Write a DNS label (length byte + string) into buf, return bytes written */
static int dns_write_label(uint8_t *buf, const char *label) {
    int len = (int)strlen(label);
    buf[0] = (uint8_t)len;
    memcpy(buf + 1, label, len);
    return 1 + len;
}

static int mdns_init(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("rtpmidi: mdns socket");
        return -1;
    }

    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("rtpmidi: mdns SO_REUSEADDR");
        close(fd);
        return -1;
    }

#ifdef SO_REUSEPORT
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
        perror("rtpmidi: mdns SO_REUSEPORT");
        close(fd);
        return -1;
    }
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(5353);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("rtpmidi: mdns bind");
        close(fd);
        return -1;
    }

    /* Join multicast group 224.0.0.251 */
    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr("224.0.0.251");
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);

    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        perror("rtpmidi: mdns multicast join");
        close(fd);
        return -1;
    }

    printf("rtpmidi: mDNS responder ready on port 5353\n");
    return fd;
}

static void mdns_respond(int mdns_fd) {
    uint8_t query[2048];
    struct sockaddr_in src;
    socklen_t src_len = sizeof(src);

    int n = recvfrom(mdns_fd, query, sizeof(query), 0,
                     (struct sockaddr *)&src, &src_len);
    if (n < 12) return;

    /* Check this is a query (QR bit = 0 in flags) */
    uint16_t flags;
    memcpy(&flags, query + 2, 2);
    flags = ntohs(flags);
    if (flags & 0x8000) return;  /* Not a query */

    /* Scan for "_apple-midi" in the query */
    int found = 0;
    for (int i = 0; i < n - 12; i++) {
        if (query[i] == 11 && i + 11 < n &&
            memcmp(query + i + 1, "_apple-midi", 11) == 0) {
            found = 1;
            break;
        }
    }
    if (!found) return;

    /* Build mDNS response */
    uint8_t resp[512];
    int off = 0;

    /* DNS header */
    uint16_t txid = 0;
    uint16_t resp_flags = htons(0x8400);  /* Response, authoritative */
    uint16_t zero = 0;
    uint16_t ancount = htons(3);

    memcpy(resp + off, &txid, 2); off += 2;
    memcpy(resp + off, &resp_flags, 2); off += 2;
    memcpy(resp + off, &zero, 2); off += 2;  /* QDCOUNT */
    memcpy(resp + off, &ancount, 2); off += 2;  /* ANCOUNT */
    memcpy(resp + off, &zero, 2); off += 2;  /* NSCOUNT */
    memcpy(resp + off, &zero, 2); off += 2;  /* ARCOUNT */

    /* Helper: write the service type labels: _apple-midi._udp.local */
    /* We'll build the common name portions we need */

    /* --- PTR record: _apple-midi._udp.local -> Move._apple-midi._udp.local --- */
    /* Name: _apple-midi._udp.local */
    int ptr_name_start = off;
    off += dns_write_label(resp + off, "_apple-midi");
    off += dns_write_label(resp + off, "_udp");
    off += dns_write_label(resp + off, "local");
    resp[off++] = 0;  /* Root label */

    /* Type PTR (12), Class IN (1) */
    uint16_t type_ptr = htons(12);
    uint16_t class_in = htons(1);
    uint32_t ttl = htonl(4500);

    memcpy(resp + off, &type_ptr, 2); off += 2;
    memcpy(resp + off, &class_in, 2); off += 2;
    memcpy(resp + off, &ttl, 4); off += 4;

    /* RDATA: Move._apple-midi._udp.local (with pointer to service type) */
    int rdlen_off = off;
    off += 2;  /* Placeholder for RDLENGTH */
    int rdata_start = off;

    off += dns_write_label(resp + off, service_name);
    /* Pointer to _apple-midi._udp.local at ptr_name_start */
    uint16_t ptr = htons(0xC000 | (uint16_t)ptr_name_start);
    memcpy(resp + off, &ptr, 2); off += 2;

    uint16_t rdlen = htons((uint16_t)(off - rdata_start));
    memcpy(resp + rdlen_off, &rdlen, 2);

    /* --- SRV record: Move._apple-midi._udp.local -> Move.local port 5004 --- */
    /* Name: Move._apple-midi._udp.local (use label + pointer) */
    off += dns_write_label(resp + off, service_name);
    memcpy(resp + off, &ptr, 2); off += 2;  /* Pointer to _apple-midi._udp.local */

    /* Type SRV (33), Class IN flush (0x8001) */
    uint16_t type_srv = htons(33);
    uint16_t class_flush = htons(0x8001);

    memcpy(resp + off, &type_srv, 2); off += 2;
    memcpy(resp + off, &class_flush, 2); off += 2;
    memcpy(resp + off, &ttl, 4); off += 4;

    /* SRV RDATA: priority(2) + weight(2) + port(2) + target */
    int srv_rdlen_off = off;
    off += 2;  /* Placeholder for RDLENGTH */
    int srv_rdata_start = off;

    uint16_t priority = 0;
    uint16_t weight = 0;
    uint16_t port = htons(RTPMIDI_CONTROL_PORT);
    memcpy(resp + off, &priority, 2); off += 2;
    memcpy(resp + off, &weight, 2); off += 2;
    memcpy(resp + off, &port, 2); off += 2;

    /* Target: Move.local */
    int target_name_off = off;
    off += dns_write_label(resp + off, service_name);
    off += dns_write_label(resp + off, "local");
    resp[off++] = 0;  /* Root label */

    uint16_t srv_rdlen = htons((uint16_t)(off - srv_rdata_start));
    memcpy(resp + srv_rdlen_off, &srv_rdlen, 2);

    /* --- A record: Move.local -> local IP --- */
    /* Name: pointer to Move.local from SRV target */
    uint16_t target_ptr = htons(0xC000 | (uint16_t)target_name_off);
    memcpy(resp + off, &target_ptr, 2); off += 2;

    /* Type A (1), Class IN flush (0x8001) */
    uint16_t type_a = htons(1);
    memcpy(resp + off, &type_a, 2); off += 2;
    memcpy(resp + off, &class_flush, 2); off += 2;
    memcpy(resp + off, &ttl, 4); off += 4;

    /* RDATA: 4-byte IPv4 address */
    uint16_t a_rdlen = htons(4);
    memcpy(resp + off, &a_rdlen, 2); off += 2;

    uint32_t local_ip = get_local_ip();
    memcpy(resp + off, &local_ip, 4); off += 4;

    /* Send to multicast */
    struct sockaddr_in mcast;
    memset(&mcast, 0, sizeof(mcast));
    mcast.sin_family = AF_INET;
    mcast.sin_addr.s_addr = inet_addr("224.0.0.251");
    mcast.sin_port = htons(5353);

    sendto(mdns_fd, resp, off, 0,
           (struct sockaddr *)&mcast, sizeof(mcast));
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

    printf("rtpmidi: sent BYE to %s:%u\n",
           inet_ntoa(session.remote_addr.sin_addr),
           ntohs(session.remote_addr.sin_port));

    session.state = SESSION_IDLE;
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(int argc, char *argv[]) {
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

    /* Initialize mDNS */
    int mdns_fd = mdns_init();

    /* Initialize session */
    memset(&session, 0, sizeof(session));
    session.local_ssrc = (uint32_t)getpid();
    session.state = SESSION_IDLE;

    printf("rtpmidi: daemon ready (SSRC=0x%08X)\n", session.local_ssrc);

    /* Main poll loop */
    struct pollfd fds[3];
    fds[0].fd = control_fd;
    fds[0].events = POLLIN;
    fds[1].fd = data_fd;
    fds[1].events = POLLIN;
    fds[2].fd = mdns_fd;
    fds[2].events = (mdns_fd >= 0) ? POLLIN : 0;

    uint8_t buf[2048];

    while (running) {
        int nfds = (mdns_fd >= 0) ? 3 : 2;
        int ret = poll(fds, nfds, 1000);

        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("rtpmidi: poll");
            break;
        }

        if (ret == 0) continue;  /* Timeout - nothing to do */

        /* Control port */
        if (fds[0].revents & POLLIN) {
            struct sockaddr_in src;
            socklen_t src_len = sizeof(src);
            int n = recvfrom(control_fd, buf, sizeof(buf), 0,
                             (struct sockaddr *)&src, &src_len);
            if (n > 0) {
                handle_control_packet(control_fd, buf, n, &src, src_len);
            }
        }

        /* Data port */
        if (fds[1].revents & POLLIN) {
            struct sockaddr_in src;
            socklen_t src_len = sizeof(src);
            int n = recvfrom(data_fd, buf, sizeof(buf), 0,
                             (struct sockaddr *)&src, &src_len);
            if (n > 0) {
                handle_data_packet(data_fd, buf, n, &src, src_len);
            }
        }

        /* mDNS */
        if (nfds > 2 && (fds[2].revents & POLLIN)) {
            mdns_respond(mdns_fd);
        }
    }

    /* Shutdown */
    printf("rtpmidi: shutting down\n");

    send_bye(control_fd);

    close(control_fd);
    close(data_fd);
    if (mdns_fd >= 0) close(mdns_fd);

    if (rtp_shm) {
        munmap(rtp_shm, sizeof(shadow_rtp_midi_t));
    }

    printf("rtpmidi: stopped\n");
    return 0;
}
