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
 * Forward declarations (implemented in later tasks)
 * ============================================================================ */

static void handle_control_packet(int sock_fd, const uint8_t *buf, int len,
                                  struct sockaddr_in *src, socklen_t src_len);
static void handle_data_packet(int sock_fd, const uint8_t *buf, int len,
                               struct sockaddr_in *src, socklen_t src_len);
static int  mdns_init(void);
static void mdns_respond(int mdns_fd);

/* ============================================================================
 * Stub implementations (to be replaced in subsequent tasks)
 * ============================================================================ */

/* TODO: implement in Task 4 - AppleMIDI session management */
static void handle_control_packet(int sock_fd, const uint8_t *buf, int len,
                                  struct sockaddr_in *src, socklen_t src_len) {
    (void)sock_fd; (void)buf; (void)len; (void)src; (void)src_len;
}

/* TODO: implement in Task 5 - RTP-MIDI payload parsing */
static void handle_data_packet(int sock_fd, const uint8_t *buf, int len,
                               struct sockaddr_in *src, socklen_t src_len) {
    (void)sock_fd; (void)buf; (void)len; (void)src; (void)src_len;
}

/* TODO: implement in Task 6 - mDNS/DNS-SD service advertisement */
static int mdns_init(void) {
    return -1;  /* No mDNS socket yet */
}

/* TODO: implement in Task 6 - mDNS/DNS-SD query responder */
static void mdns_respond(int mdns_fd) {
    (void)mdns_fd;
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
