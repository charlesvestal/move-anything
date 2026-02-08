# RTP-MIDI Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Enable wireless MIDI to Move via RTP-MIDI (Apple MIDI), with a daemon that receives MIDI over WiFi and injects it into the hardware mailbox as cable 2 external MIDI.

**Architecture:** A standalone C daemon handles mDNS advertisement, AppleMIDI session management, and RTP-MIDI parsing. It writes USB-MIDI packets to a shared memory segment. The shim reads that segment and merges packets into `MIDI_IN` so both stock Move and Move Anything see them. A toggle in Master FX Settings controls the daemon lifecycle.

**Tech Stack:** C (POSIX sockets, shm), cross-compiled for ARM via Docker. No external library dependencies.

---

### Task 1: Define shared memory structure for RTP-MIDI injection

**Files:**
- Modify: `src/host/shadow_constants.h`

**Step 1: Add SHM name constant and buffer size**

In `shadow_constants.h`, after the `SHM_SHADOW_SCREENREADER` define (~line 29), add:

```c
#define SHM_SHADOW_RTP_MIDI   "/move-shadow-rtp-midi"  /* RTP-MIDI input injection */
```

After the `SHADOW_SCREENREADER_BUFFER_SIZE` define (~line 41), add:

```c
#define SHADOW_RTP_MIDI_BUFFER_SIZE 256  /* RTP-MIDI injection buffer (64 USB-MIDI packets) */
```

**Step 2: Add the shared memory struct**

After the `shadow_screenreader_t` typedef (~line 155), add:

```c
/*
 * RTP-MIDI injection structure.
 * External rtpmidi-daemon writes USB-MIDI packets here.
 * Shim reads and merges into MIDI_IN mailbox region.
 * Same pattern as shadow_midi_out_t but for input direction.
 */
typedef struct shadow_rtp_midi_t {
    volatile uint8_t write_idx;      /* Daemon increments after writing */
    volatile uint8_t ready;          /* Toggle to signal new data */
    volatile uint8_t reserved[2];
    uint8_t buffer[SHADOW_RTP_MIDI_BUFFER_SIZE];  /* USB-MIDI packets (4 bytes each) */
} shadow_rtp_midi_t;
```

**Step 3: Commit**

```bash
git add src/host/shadow_constants.h
git commit -m "feat(rtp-midi): add shared memory structure for MIDI injection"
```

---

### Task 2: Add shim shared memory setup and injection function

**Files:**
- Modify: `src/move_anything_shim.c`

**Step 1: Add shm variables**

Near the existing `shadow_midi_out_shm` declaration (~line 5363), add:

```c
static shadow_rtp_midi_t *shadow_rtp_midi_shm = NULL;
static uint8_t last_shadow_rtp_midi_ready = 0;
```

Near the existing `shm_midi_out_fd` declaration (~line 5396), add:

```c
static int shm_rtp_midi_fd = -1;
```

**Step 2: Add shm open/mmap in shadow init**

In the shadow shared memory initialization function (after the `shm_midi_out_fd` block, ~line 5620), add:

```c
    /* Create/open RTP-MIDI injection shared memory */
    shm_rtp_midi_fd = shm_open(SHM_SHADOW_RTP_MIDI, O_CREAT | O_RDWR, 0666);
    if (shm_rtp_midi_fd >= 0) {
        ftruncate(shm_rtp_midi_fd, sizeof(shadow_rtp_midi_t));
        shadow_rtp_midi_shm = (shadow_rtp_midi_t *)mmap(NULL, sizeof(shadow_rtp_midi_t),
                                                         PROT_READ | PROT_WRITE,
                                                         MAP_SHARED, shm_rtp_midi_fd, 0);
        if (shadow_rtp_midi_shm == MAP_FAILED) {
            shadow_rtp_midi_shm = NULL;
            printf("Shadow: Failed to mmap rtp_midi shm\n");
        } else {
            memset(shadow_rtp_midi_shm, 0, sizeof(shadow_rtp_midi_t));
        }
    } else {
        printf("Shadow: Failed to create rtp_midi shm\n");
    }
```

Update the init complete printf to include `rtp_midi=%p`.

**Step 3: Add injection function**

After `shadow_inject_ui_midi_out` (~line 6019), add:

```c
/* Inject RTP-MIDI packets into shadow_mailbox MIDI_IN region.
 * Called after hardware MIDI_IN has been copied to shadow_mailbox,
 * so we merge wireless MIDI alongside wired USB MIDI. */
static void shadow_inject_rtp_midi(void) {
    if (!shadow_rtp_midi_shm) return;
    if (shadow_rtp_midi_shm->ready == last_shadow_rtp_midi_ready) return;

    last_shadow_rtp_midi_ready = shadow_rtp_midi_shm->ready;

    uint8_t *midi_in = shadow_mailbox + MIDI_IN_OFFSET;

    for (int i = 0; i < shadow_rtp_midi_shm->write_idx && i < SHADOW_RTP_MIDI_BUFFER_SIZE; i += 4) {
        uint8_t cin = shadow_rtp_midi_shm->buffer[i] & 0x0F;
        if (cin < 0x08 || cin > 0x0E) continue;  /* Skip invalid CIN */

        /* Find an empty slot in MIDI_IN */
        int hw_offset = 0;
        while (hw_offset < MIDI_BUFFER_SIZE) {
            if (midi_in[hw_offset] == 0 && midi_in[hw_offset+1] == 0 &&
                midi_in[hw_offset+2] == 0 && midi_in[hw_offset+3] == 0) {
                break;
            }
            hw_offset += 4;
        }
        if (hw_offset >= MIDI_BUFFER_SIZE) break;  /* Buffer full */

        memcpy(&midi_in[hw_offset], &shadow_rtp_midi_shm->buffer[i], 4);
    }

    /* Clear after processing */
    shadow_rtp_midi_shm->write_idx = 0;
    memset(shadow_rtp_midi_shm->buffer, 0, SHADOW_RTP_MIDI_BUFFER_SIZE);
}
```

**Step 4: Call injection in ioctl handler**

In the ioctl handler, after the MIDI_IN copy from hardware to shadow_mailbox completes (~line 7806, after the `memcpy(sh_midi, hw_midi, MIDI_BUFFER_SIZE)` else branch), add:

```c
        /* Inject any RTP-MIDI packets into MIDI_IN */
        shadow_inject_rtp_midi();
```

This should be placed after the MIDI_IN filtering/copying block but before the shift+menu detection block.

**Step 5: Commit**

```bash
git add src/move_anything_shim.c
git commit -m "feat(rtp-midi): add shim shared memory and injection into MIDI_IN"
```

---

### Task 3: Write the RTP-MIDI daemon - shared memory and socket setup

**Files:**
- Create: `src/rtpmidi/rtpmidi_daemon.c`
- Create: `src/rtpmidi/rtpmidi.h`

**Step 1: Create header with protocol constants**

Create `src/rtpmidi/rtpmidi.h`:

```c
#ifndef RTPMIDI_H
#define RTPMIDI_H

#include <stdint.h>

/* AppleMIDI signature */
#define APPLEMIDI_SIGNATURE 0xFFFF

/* AppleMIDI command codes (network byte order) */
#define APPLEMIDI_CMD_IN   0x494E  /* Invitation */
#define APPLEMIDI_CMD_OK   0x4F4B  /* Accept */
#define APPLEMIDI_CMD_NO   0x4E4F  /* Reject */
#define APPLEMIDI_CMD_BY   0x4259  /* Bye */
#define APPLEMIDI_CMD_CK   0x434B  /* Clock sync */

/* RTP */
#define RTP_PAYLOAD_TYPE   0x61   /* 97 - standard for RTP-MIDI */
#define RTP_VERSION        2

/* Default ports */
#define RTPMIDI_CONTROL_PORT 5004
#define RTPMIDI_DATA_PORT    5005

/* Session state */
typedef enum {
    SESSION_IDLE,
    SESSION_CONNECTED
} session_state_t;

typedef struct {
    session_state_t state;
    uint32_t remote_ssrc;
    uint32_t local_ssrc;
    uint32_t initiator_token;
    struct sockaddr_in remote_addr;
    socklen_t remote_addr_len;
} rtpmidi_session_t;

/* USB-MIDI CIN values */
static inline uint8_t midi_status_to_cin(uint8_t status) {
    return (status >> 4) & 0x0F;
}

/* Format a MIDI message as a cable-2 USB-MIDI packet */
static inline void format_usb_midi_packet(uint8_t *out, uint8_t status, uint8_t d1, uint8_t d2) {
    uint8_t cin = midi_status_to_cin(status);
    out[0] = 0x20 | cin;  /* Cable 2 + CIN */
    out[1] = status;
    out[2] = d1;
    out[3] = d2;
}

#endif /* RTPMIDI_H */
```

**Step 2: Create daemon main with shm and socket setup**

Create `src/rtpmidi/rtpmidi_daemon.c` with the following initial structure:

```c
/*
 * rtpmidi-daemon: RTP-MIDI receiver for Move
 *
 * Receives MIDI over WiFi via the Apple MIDI / RTP-MIDI protocol,
 * writes USB-MIDI packets to shared memory for the shim to inject
 * into the hardware MIDI_IN mailbox.
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

static volatile int running = 1;
static shadow_rtp_midi_t *rtp_shm = NULL;
static rtpmidi_session_t session;
static const char *service_name = "Move";

static void signal_handler(int sig) {
    (void)sig;
    running = 0;
}

/* ---- Shared Memory ---- */

static int shm_init(void) {
    int fd = shm_open(SHM_SHADOW_RTP_MIDI, O_RDWR, 0666);
    if (fd < 0) {
        /* Try creating it if shim hasn't yet */
        fd = shm_open(SHM_SHADOW_RTP_MIDI, O_CREAT | O_RDWR, 0666);
        if (fd < 0) {
            perror("shm_open");
            return -1;
        }
        ftruncate(fd, sizeof(shadow_rtp_midi_t));
    }
    rtp_shm = (shadow_rtp_midi_t *)mmap(NULL, sizeof(shadow_rtp_midi_t),
                                         PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (rtp_shm == MAP_FAILED) {
        perror("mmap");
        rtp_shm = NULL;
        close(fd);
        return -1;
    }
    return 0;
}

/* Write a USB-MIDI packet to the shared memory buffer */
static void shm_write_midi(uint8_t status, uint8_t d1, uint8_t d2) {
    if (!rtp_shm) return;
    int idx = rtp_shm->write_idx;
    if (idx + 4 > SHADOW_RTP_MIDI_BUFFER_SIZE) return;  /* Buffer full */

    format_usb_midi_packet(&rtp_shm->buffer[idx], status, d1, d2);
    rtp_shm->write_idx = idx + 4;
    rtp_shm->ready++;
}

/* ---- UDP Sockets ---- */

static int create_udp_socket(int port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    return fd;
}

/* Forward declarations - implemented in subsequent tasks */
static void handle_control_packet(int control_fd, uint8_t *buf, int len,
                                   struct sockaddr_in *from, socklen_t from_len);
static void handle_data_packet(uint8_t *buf, int len);
static int mdns_init(void);
static void mdns_respond(int mdns_fd);

/* ---- Main Loop ---- */

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    printf("rtpmidi-daemon: starting (service name: %s)\n", service_name);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    if (shm_init() < 0) {
        fprintf(stderr, "Failed to init shared memory\n");
        return 1;
    }

    int control_fd = create_udp_socket(RTPMIDI_CONTROL_PORT);
    int data_fd = create_udp_socket(RTPMIDI_DATA_PORT);
    int mdns_fd = mdns_init();

    if (control_fd < 0 || data_fd < 0) {
        fprintf(stderr, "Failed to create sockets\n");
        return 1;
    }

    memset(&session, 0, sizeof(session));
    session.local_ssrc = (uint32_t)getpid();

    printf("rtpmidi-daemon: listening on ports %d/%d\n",
           RTPMIDI_CONTROL_PORT, RTPMIDI_DATA_PORT);

    struct pollfd fds[3];
    int nfds = 2;
    fds[0].fd = control_fd;
    fds[0].events = POLLIN;
    fds[1].fd = data_fd;
    fds[1].events = POLLIN;
    if (mdns_fd >= 0) {
        fds[2].fd = mdns_fd;
        fds[2].events = POLLIN;
        nfds = 3;
    }

    while (running) {
        int ret = poll(fds, nfds, 1000);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (fds[0].revents & POLLIN) {
            uint8_t buf[1024];
            struct sockaddr_in from;
            socklen_t from_len = sizeof(from);
            int n = recvfrom(control_fd, buf, sizeof(buf), 0,
                            (struct sockaddr *)&from, &from_len);
            if (n > 0) handle_control_packet(control_fd, buf, n, &from, from_len);
        }

        if (fds[1].revents & POLLIN) {
            uint8_t buf[1024];
            int n = recvfrom(data_fd, buf, sizeof(buf), 0, NULL, NULL);
            if (n > 0) handle_data_packet(buf, n);
        }

        if (nfds > 2 && (fds[2].revents & POLLIN)) {
            mdns_respond(mdns_fd);
        }
    }

    printf("rtpmidi-daemon: shutting down\n");

    /* Send BYE to connected client */
    if (session.state == SESSION_CONNECTED) {
        uint8_t bye[16];
        uint16_t sig = htons(APPLEMIDI_SIGNATURE);
        uint16_t cmd = htons(APPLEMIDI_CMD_BY);
        uint32_t ver = htonl(2);
        uint32_t token = htonl(session.initiator_token);
        uint32_t ssrc = htonl(session.local_ssrc);
        memcpy(bye, &sig, 2);
        memcpy(bye + 2, &cmd, 2);
        memcpy(bye + 4, &ver, 4);
        memcpy(bye + 8, &token, 4);
        memcpy(bye + 12, &ssrc, 4);
        sendto(control_fd, bye, 16, 0,
               (struct sockaddr *)&session.remote_addr, session.remote_addr_len);
    }

    close(control_fd);
    close(data_fd);
    if (mdns_fd >= 0) close(mdns_fd);

    return 0;
}
```

**Step 3: Commit**

```bash
git add src/rtpmidi/rtpmidi.h src/rtpmidi/rtpmidi_daemon.c
git commit -m "feat(rtp-midi): daemon skeleton with shm and socket setup"
```

---

### Task 4: Implement AppleMIDI session handling

**Files:**
- Modify: `src/rtpmidi/rtpmidi_daemon.c`

**Step 1: Implement handle_control_packet**

Replace the forward declaration of `handle_control_packet` with the full implementation. This function handles the AppleMIDI command protocol on the control port:

```c
/* Handle AppleMIDI command packet on control port */
static void handle_control_packet(int control_fd, uint8_t *buf, int len,
                                   struct sockaddr_in *from, socklen_t from_len) {
    if (len < 8) return;

    uint16_t sig, cmd;
    memcpy(&sig, buf, 2);
    memcpy(&cmd, buf + 2, 2);
    sig = ntohs(sig);
    cmd = ntohs(cmd);

    if (sig != APPLEMIDI_SIGNATURE) return;

    if (cmd == APPLEMIDI_CMD_IN) {
        /* Invitation - accept if no active session or same client */
        if (len < 16) return;

        uint32_t version, token, ssrc;
        memcpy(&version, buf + 4, 4); version = ntohl(version);
        memcpy(&token, buf + 8, 4);   token = ntohl(token);
        memcpy(&ssrc, buf + 12, 4);   ssrc = ntohl(ssrc);

        /* Extract name if present */
        char remote_name[64] = "";
        if (len > 16) {
            int name_len = len - 16;
            if (name_len > 63) name_len = 63;
            memcpy(remote_name, buf + 16, name_len);
            remote_name[name_len] = '\0';
        }

        printf("rtpmidi: invitation from %s (SSRC=%u)\n",
               remote_name[0] ? remote_name : "unknown", ssrc);

        /* Send OK response */
        uint8_t resp[256];
        uint16_t r_sig = htons(APPLEMIDI_SIGNATURE);
        uint16_t r_cmd = htons(APPLEMIDI_CMD_OK);
        uint32_t r_ver = htonl(2);
        uint32_t r_token = htonl(token);
        uint32_t r_ssrc = htonl(session.local_ssrc);
        memcpy(resp, &r_sig, 2);
        memcpy(resp + 2, &r_cmd, 2);
        memcpy(resp + 4, &r_ver, 4);
        memcpy(resp + 8, &r_token, 4);
        memcpy(resp + 12, &r_ssrc, 4);
        int resp_len = 16;

        /* Append our name */
        int name_len = strlen(service_name) + 1;
        memcpy(resp + 16, service_name, name_len);
        resp_len += name_len;

        sendto(control_fd, resp, resp_len, 0,
               (struct sockaddr *)from, from_len);

        session.state = SESSION_CONNECTED;
        session.remote_ssrc = ssrc;
        session.initiator_token = token;
        memcpy(&session.remote_addr, from, from_len);
        session.remote_addr_len = from_len;

        printf("rtpmidi: session established with %s\n",
               remote_name[0] ? remote_name : "unknown");

    } else if (cmd == APPLEMIDI_CMD_CK) {
        /* Clock sync - respond with timestamps */
        if (len < 36) return;

        uint32_t ssrc;
        memcpy(&ssrc, buf + 4, 4);
        uint8_t count = buf[8];

        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t now = (uint64_t)ts.tv_sec * 10000 + ts.tv_nsec / 100000;

        if (count == 0) {
            /* Respond with count=1, copy sender's ts1, add our ts2 */
            uint8_t resp[36];
            memcpy(resp, buf, 36);
            uint16_t r_sig = htons(APPLEMIDI_SIGNATURE);
            uint16_t r_cmd = htons(APPLEMIDI_CMD_CK);
            uint32_t r_ssrc = htonl(session.local_ssrc);
            memcpy(resp, &r_sig, 2);
            memcpy(resp + 2, &r_cmd, 2);
            memcpy(resp + 4, &r_ssrc, 4);
            resp[8] = 1;  /* count = 1 */
            /* ts2 at offset 20 */
            uint64_t ts2_be = htobe64(now);
            memcpy(resp + 20, &ts2_be, 8);
            sendto(control_fd, resp, 36, 0,
                   (struct sockaddr *)from, from_len);
        } else if (count == 2) {
            /* Final sync - no response needed, sync complete */
        }

    } else if (cmd == APPLEMIDI_CMD_BY) {
        /* Bye - tear down session */
        if (len < 16) return;

        uint32_t ssrc;
        memcpy(&ssrc, buf + 12, 4);
        ssrc = ntohl(ssrc);

        if (session.state == SESSION_CONNECTED && ssrc == session.remote_ssrc) {
            printf("rtpmidi: session ended by remote\n");
            session.state = SESSION_IDLE;
            session.remote_ssrc = 0;
        }
    }
}
```

**Step 2: Commit**

```bash
git add src/rtpmidi/rtpmidi_daemon.c
git commit -m "feat(rtp-midi): implement AppleMIDI session protocol (IN/OK/CK/BY)"
```

---

### Task 5: Implement RTP-MIDI packet parsing

**Files:**
- Modify: `src/rtpmidi/rtpmidi_daemon.c`

**Step 1: Implement handle_data_packet**

Replace the forward declaration of `handle_data_packet` with the full implementation. This parses RTP headers and extracts MIDI messages from the MIDI command section:

```c
/* Parse RTP-MIDI data packet and extract MIDI messages */
static void handle_data_packet(uint8_t *buf, int len) {
    if (session.state != SESSION_CONNECTED) return;
    if (len < 13) return;  /* Minimum: 12-byte RTP header + 1 byte MIDI cmd section */

    /* Validate RTP header */
    uint8_t version = (buf[0] >> 6) & 0x03;
    if (version != RTP_VERSION) return;

    uint8_t pt = buf[1] & 0x7F;
    if (pt != RTP_PAYLOAD_TYPE) return;

    /* MIDI command section starts after RTP header (12 bytes) */
    int offset = 12;
    if (offset >= len) return;

    uint8_t flags = buf[offset];
    int B = (flags >> 7) & 1;  /* Long header */
    int J = (flags >> 6) & 1;  /* Journal present */
    int Z = (flags >> 5) & 1;  /* Zero delta-time for first */
    int midi_list_len;

    if (B) {
        /* Long header: 12-bit length across flags byte and next byte */
        if (offset + 1 >= len) return;
        midi_list_len = ((flags & 0x0F) << 8) | buf[offset + 1];
        offset += 2;
    } else {
        /* Short header: 4-bit length in flags byte */
        midi_list_len = flags & 0x0F;
        offset += 1;
    }

    if (midi_list_len == 0) return;
    if (offset + midi_list_len > len) return;

    /* Parse MIDI messages from the MIDI list */
    int midi_end = offset + midi_list_len;
    int first_msg = 1;
    uint8_t running_status = 0;

    while (offset < midi_end) {
        /* Skip delta-time (variable-length quantity) unless Z flag on first message */
        if (!(first_msg && Z)) {
            while (offset < midi_end && (buf[offset] & 0x80)) {
                offset++;  /* Skip continuation bytes */
            }
            if (offset < midi_end) offset++;  /* Skip final byte */
        }
        first_msg = 0;

        if (offset >= midi_end) break;

        /* Read MIDI message */
        uint8_t status, d1 = 0, d2 = 0;

        if (buf[offset] & 0x80) {
            /* New status byte */
            status = buf[offset++];
            running_status = status;
        } else {
            /* Running status */
            status = running_status;
        }

        if (status == 0) break;

        uint8_t type = status & 0xF0;

        /* Determine message length based on status type */
        switch (type) {
            case 0x80:  /* Note Off */
            case 0x90:  /* Note On */
            case 0xA0:  /* Poly Aftertouch */
            case 0xB0:  /* Control Change */
            case 0xE0:  /* Pitch Bend */
                if (offset + 1 >= midi_end) goto done;
                d1 = buf[offset++];
                d2 = buf[offset++];
                shm_write_midi(status, d1, d2);
                break;
            case 0xC0:  /* Program Change */
            case 0xD0:  /* Channel Pressure */
                if (offset >= midi_end) goto done;
                d1 = buf[offset++];
                shm_write_midi(status, d1, 0);
                break;
            case 0xF0:  /* System messages - skip */
                if (status == 0xF8 || status == 0xFA || status == 0xFB || status == 0xFC) {
                    /* Realtime: clock, start, continue, stop - 1 byte */
                    shm_write_midi(status, 0, 0);
                } else {
                    /* Skip other system messages (sysex etc) */
                    while (offset < midi_end && buf[offset] != 0xF7) offset++;
                    if (offset < midi_end) offset++;  /* Skip F7 */
                }
                break;
            default:
                goto done;
        }
    }
done:
    (void)J;  /* Journal intentionally not parsed */
}
```

**Step 2: Commit**

```bash
git add src/rtpmidi/rtpmidi_daemon.c
git commit -m "feat(rtp-midi): implement RTP-MIDI packet parsing and MIDI extraction"
```

---

### Task 6: Implement mDNS service advertisement

**Files:**
- Modify: `src/rtpmidi/rtpmidi_daemon.c`

**Step 1: Implement mDNS responder**

Replace the forward declarations of `mdns_init` and `mdns_respond` with full implementations. This is a minimal mDNS responder that answers queries for `_apple-midi._udp.local`:

```c
/* ---- mDNS Service Advertisement ---- */

#include <net/if.h>
#include <ifaddrs.h>

#define MDNS_PORT 5353
#define MDNS_ADDR "224.0.0.251"

/* Get the first non-loopback IPv4 address */
static uint32_t get_local_ip(void) {
    struct ifaddrs *ifaddr, *ifa;
    uint32_t ip = 0;

    if (getifaddrs(&ifaddr) == -1) return 0;

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (ifa->ifa_flags & IFF_LOOPBACK) continue;

        ip = ((struct sockaddr_in *)ifa->ifa_addr)->sin_addr.s_addr;
        break;
    }
    freeifaddrs(ifaddr);
    return ip;
}

/* Write a DNS name label (e.g., "_apple-midi" -> \x0b_apple-midi) */
static int dns_write_label(uint8_t *buf, const char *label) {
    int len = strlen(label);
    buf[0] = (uint8_t)len;
    memcpy(buf + 1, label, len);
    return len + 1;
}

/* Write the full service name: _apple-midi._udp.local */
static int dns_write_service_name(uint8_t *buf) {
    int off = 0;
    off += dns_write_label(buf + off, "_apple-midi");
    off += dns_write_label(buf + off, "_udp");
    off += dns_write_label(buf + off, "local");
    buf[off++] = 0;  /* Root label */
    return off;
}

/* Write instance name: Move._apple-midi._udp.local */
static int dns_write_instance_name(uint8_t *buf) {
    int off = 0;
    off += dns_write_label(buf + off, service_name);
    off += dns_write_label(buf + off, "_apple-midi");
    off += dns_write_label(buf + off, "_udp");
    off += dns_write_label(buf + off, "local");
    buf[off++] = 0;
    return off;
}

/* Write hostname: Move.local */
static int dns_write_hostname(uint8_t *buf) {
    int off = 0;
    off += dns_write_label(buf + off, service_name);
    off += dns_write_label(buf + off, "local");
    buf[off++] = 0;
    return off;
}

static int mdns_init(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("mdns socket"); return -1; }

    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    #ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
    #endif

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(MDNS_PORT);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("mdns bind");
        close(fd);
        return -1;
    }

    /* Join multicast group */
    struct ip_mreq mreq = {0};
    mreq.imr_multiaddr.s_addr = inet_addr(MDNS_ADDR);
    mreq.imr_interface.s_addr = INADDR_ANY;
    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        perror("mdns multicast join");
        close(fd);
        return -1;
    }

    printf("rtpmidi: mDNS responder listening\n");

    /* Send initial announcement */
    /* (will be handled by mdns_announce called from main after init) */
    return fd;
}

static void mdns_respond(int mdns_fd) {
    uint8_t query[1500];
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    int n = recvfrom(mdns_fd, query, sizeof(query), 0,
                     (struct sockaddr *)&from, &from_len);
    if (n < 12) return;

    /* Parse DNS header */
    uint16_t flags;
    memcpy(&flags, query + 2, 2);
    flags = ntohs(flags);
    if (flags & 0x8000) return;  /* Skip responses, only handle queries */

    uint16_t qdcount;
    memcpy(&qdcount, query + 4, 2);
    qdcount = ntohs(qdcount);
    if (qdcount == 0) return;

    /* Check if query is for _apple-midi._udp.local (PTR, type 12) */
    /* Simple check: look for "_apple-midi" in the query */
    int found = 0;
    for (int i = 12; i < n - 12; i++) {
        if (query[i] == 11 && memcmp(query + i + 1, "_apple-midi", 11) == 0) {
            found = 1;
            break;
        }
    }
    if (!found) return;

    /* Build response with PTR + SRV + A records */
    uint8_t resp[512];
    int off = 0;

    /* DNS header - response with authority */
    uint16_t txid = 0;
    uint16_t r_flags = htons(0x8400);  /* Response, authoritative */
    uint16_t zero = 0;
    uint16_t ancount = htons(3);  /* PTR + SRV + A */
    memcpy(resp + off, &txid, 2); off += 2;
    memcpy(resp + off, &r_flags, 2); off += 2;
    memcpy(resp + off, &zero, 2); off += 2;   /* QDCOUNT */
    memcpy(resp + off, &ancount, 2); off += 2; /* ANCOUNT */
    memcpy(resp + off, &zero, 2); off += 2;   /* NSCOUNT */
    memcpy(resp + off, &zero, 2); off += 2;   /* ARCOUNT */

    /* PTR record: _apple-midi._udp.local -> Move._apple-midi._udp.local */
    int svc_name_start = off;
    off += dns_write_service_name(resp + off);
    uint16_t ptr_type = htons(12);   /* PTR */
    uint16_t ptr_class = htons(1);   /* IN */
    uint32_t ttl = htonl(4500);
    memcpy(resp + off, &ptr_type, 2); off += 2;
    memcpy(resp + off, &ptr_class, 2); off += 2;
    memcpy(resp + off, &ttl, 4); off += 4;
    /* RDLENGTH placeholder */
    int rdlen_off = off;
    off += 2;
    int rd_start = off;
    off += dns_write_instance_name(resp + off);
    uint16_t rdlen = htons(off - rd_start);
    memcpy(resp + rdlen_off, &rdlen, 2);

    /* SRV record: Move._apple-midi._udp.local -> Move.local port 5004 */
    int inst_name_start = off;
    off += dns_write_instance_name(resp + off);
    uint16_t srv_type = htons(33);   /* SRV */
    uint16_t srv_class = htons(0x8001);  /* IN, cache flush */
    memcpy(resp + off, &srv_type, 2); off += 2;
    memcpy(resp + off, &srv_class, 2); off += 2;
    memcpy(resp + off, &ttl, 4); off += 4;
    rdlen_off = off;
    off += 2;
    rd_start = off;
    uint16_t priority = 0, weight = 0;
    uint16_t port = htons(RTPMIDI_CONTROL_PORT);
    memcpy(resp + off, &priority, 2); off += 2;
    memcpy(resp + off, &weight, 2); off += 2;
    memcpy(resp + off, &port, 2); off += 2;
    off += dns_write_hostname(resp + off);
    rdlen = htons(off - rd_start);
    memcpy(resp + rdlen_off, &rdlen, 2);

    /* A record: Move.local -> IP address */
    off += dns_write_hostname(resp + off);
    uint16_t a_type = htons(1);      /* A */
    uint16_t a_class = htons(0x8001); /* IN, cache flush */
    memcpy(resp + off, &a_type, 2); off += 2;
    memcpy(resp + off, &a_class, 2); off += 2;
    memcpy(resp + off, &ttl, 4); off += 4;
    uint16_t a_rdlen = htons(4);
    memcpy(resp + off, &a_rdlen, 2); off += 2;
    uint32_t ip = get_local_ip();
    memcpy(resp + off, &ip, 4); off += 4;

    /* Send to multicast */
    struct sockaddr_in mcast = {0};
    mcast.sin_family = AF_INET;
    mcast.sin_addr.s_addr = inet_addr(MDNS_ADDR);
    mcast.sin_port = htons(MDNS_PORT);
    sendto(mdns_fd, resp, off, 0, (struct sockaddr *)&mcast, sizeof(mcast));
}
```

**Step 2: Commit**

```bash
git add src/rtpmidi/rtpmidi_daemon.c
git commit -m "feat(rtp-midi): implement mDNS responder for Apple MIDI service discovery"
```

---

### Task 7: Add daemon to build system

**Files:**
- Modify: `scripts/build.sh`

**Step 1: Add daemon compilation to build.sh**

After the Shadow UI build block (~line 165, after the `shadow_ui` gcc command), add:

```bash
echo "Building RTP-MIDI daemon..."

# Build RTP-MIDI daemon (receives MIDI over WiFi, injects into mailbox)
"${CROSS_PREFIX}gcc" -g -O3 \
    src/rtpmidi/rtpmidi_daemon.c \
    -o build/rtpmidi-daemon \
    -Isrc -Isrc/host \
    -lrt
```

**Step 2: Verify it builds**

Run: `./scripts/build.sh`

Expected: Build succeeds, `build/rtpmidi-daemon` is created.

**Step 3: Commit**

```bash
git add scripts/build.sh
git commit -m "build: add rtpmidi-daemon to cross-compilation build"
```

---

### Task 8: Add daemon to deployment

**Files:**
- Modify: `scripts/install.sh`
- Modify: `scripts/package.sh`

**Step 1: Add rtpmidi-daemon to package.sh**

Find where `shadow_ui` is copied to the dist directory and add a line to copy `rtpmidi-daemon`:

```bash
cp build/rtpmidi-daemon "$DIST_DIR/"
```

**Step 2: Add rtpmidi-daemon to install.sh**

Find where `shadow_ui` is deployed to the device and add similar lines for `rtpmidi-daemon`. It should be installed to `/data/UserData/move-anything/` alongside the other binaries.

**Step 3: Commit**

```bash
git add scripts/install.sh scripts/package.sh
git commit -m "build: add rtpmidi-daemon to packaging and deployment"
```

---

### Task 9: Add daemon lifecycle management to the shim

**Files:**
- Modify: `src/move_anything_shim.c`

**Step 1: Add daemon PID tracking**

Near the other shadow state variables, add:

```c
static pid_t rtpmidi_daemon_pid = -1;
static int rtpmidi_enabled = 0;
```

**Step 2: Add start/stop functions**

Near `shim_run_command` (~line 2433), add:

```c
/* Start the RTP-MIDI daemon */
static void rtpmidi_start(void) {
    if (rtpmidi_daemon_pid > 0) return;  /* Already running */

    pid_t pid = fork();
    if (pid < 0) {
        printf("rtpmidi: fork failed: %s\n", strerror(errno));
        return;
    }
    if (pid == 0) {
        /* Child - exec the daemon */
        execl("/data/UserData/move-anything/rtpmidi-daemon",
              "rtpmidi-daemon", (char *)NULL);
        _exit(127);
    }
    rtpmidi_daemon_pid = pid;
    rtpmidi_enabled = 1;
    printf("rtpmidi: daemon started (pid %d)\n", pid);
}

/* Stop the RTP-MIDI daemon */
static void rtpmidi_stop(void) {
    if (rtpmidi_daemon_pid <= 0) return;

    kill(rtpmidi_daemon_pid, SIGTERM);
    /* Non-blocking wait, daemon will exit */
    int status;
    waitpid(rtpmidi_daemon_pid, &status, WNOHANG);
    printf("rtpmidi: daemon stopped (pid %d)\n", rtpmidi_daemon_pid);
    rtpmidi_daemon_pid = -1;
    rtpmidi_enabled = 0;
}
```

**Step 3: Add param handler for rtpmidi_enabled**

In the master FX param handling section of the ioctl handler (where `resample_bridge` is handled, ~line 4700), add a SET handler:

```c
            if (!has_slot_prefix && strcmp(param_key, "rtpmidi_enabled") == 0) {
                int enable = (shadow_param->value[0] == '1');
                if (enable && !rtpmidi_enabled) {
                    rtpmidi_start();
                } else if (!enable && rtpmidi_enabled) {
                    rtpmidi_stop();
                }
                shadow_param->error = 0;
                shadow_param->result_len = 0;
            }
```

And a GET handler:

```c
            if (!has_slot_prefix && strcmp(param_key, "rtpmidi_enabled") == 0) {
                shadow_param->result_len = snprintf(shadow_param->value,
                    SHADOW_PARAM_VALUE_LEN, "%d", rtpmidi_enabled);
                shadow_param->error = 0;
            }
```

**Step 4: Add config file read on startup**

In the shadow init function, after existing initialization, read `shadow_config.json` for `rtpmidi_enabled`. If true, call `rtpmidi_start()`.

Note: Look at how the shadow UI reads `shadow_config.json` for the config file path and parsing pattern. The shim may need a simple JSON parse or just check for a flag file at `/data/UserData/move-anything/rtpmidi_on`. Using a flag file is simpler and consistent with other shim feature flags (like `shadow_midi_ch3_only`). Choose whichever pattern the shadow UI already uses for persisting this setting.

**Step 5: Add cleanup on shadow shutdown**

In the shadow cleanup/exit path, call `rtpmidi_stop()` to ensure the daemon is killed when Move Anything exits.

**Step 6: Commit**

```bash
git add src/move_anything_shim.c
git commit -m "feat(rtp-midi): add daemon lifecycle management (start/stop/config)"
```

---

### Task 10: Add UI toggle in Master FX Settings

**Files:**
- Modify: `src/shadow/shadow_ui.js`

**Step 1: Add state variable**

Near the other Master FX state variables (~line 438), add:

```javascript
let rtpMidiEnabled = false;
```

**Step 2: Add to settings items**

In `MASTER_FX_SETTINGS_ITEMS_BASE` (~line 449), add before the `[Save]` action:

```javascript
    { key: "rtpmidi_enabled", label: "RTP-MIDI", type: "bool" },
```

**Step 3: Add value getter**

In `getMasterFxSettingValue()` (the function with the `if (setting.key === ...)` chain, ~line 4610), add:

```javascript
    if (setting.key === "rtpmidi_enabled") {
        /* Read current state from shim */
        const val = shadow_get_param(0, "master_fx:rtpmidi_enabled");
        rtpMidiEnabled = (val === "1");
        return rtpMidiEnabled ? "On" : "Off";
    }
```

**Step 4: Add value adjuster**

In `adjustMasterFxSetting()` (~line 4648), add before the closing brace:

```javascript
    if (setting.key === "rtpmidi_enabled") {
        rtpMidiEnabled = !rtpMidiEnabled;
        setSlotParam(0, "rtpmidi_enabled", rtpMidiEnabled ? "1" : "0");
        return;
    }
```

Note: `setSlotParam(0, key, val)` calls `shadow_set_param(0, "master_fx:" + key, val)` which routes to the shim's param handler.

**Step 5: Commit**

```bash
git add src/shadow/shadow_ui.js
git commit -m "feat(rtp-midi): add RTP-MIDI toggle to Master FX Settings UI"
```

---

### Task 11: Build, deploy, and test

**Step 1: Build**

```bash
./scripts/build.sh
```

Expected: All binaries compile including `build/rtpmidi-daemon`.

**Step 2: Deploy to Move**

```bash
./scripts/install.sh local --skip-modules
```

**Step 3: Test on device**

1. SSH into Move: `ssh ableton@move.local`
2. Check that `rtpmidi-daemon` binary exists: `ls -la /data/UserData/move-anything/rtpmidi-daemon`
3. Enter shadow mode (Shift+Vol+Knob1)
4. Navigate to Master FX Settings (Shift+Vol+Menu, go to Settings)
5. Find "RTP-MIDI" toggle, turn it On
6. On Mac: Open Audio MIDI Setup > Window > Show MIDI Studio > Network
7. Look for "Move" in the directory
8. Connect and send MIDI notes
9. Verify MIDI arrives in the signal chain

**Step 4: Test toggle off**

1. Toggle RTP-MIDI Off in Master FX Settings
2. Verify daemon stops: `ps aux | grep rtpmidi` on the Move
3. Verify "Move" disappears from macOS MIDI Network Setup

**Step 5: Commit any fixes discovered during testing**

---

### Task 12: Final review and cleanup

**Step 1: Review all changes**

```bash
git log --oneline main..rtp-midi
git diff main..rtp-midi --stat
```

**Step 2: Verify no debug artifacts remain**

Check for any leftover `printf` debugging, hardcoded test values, or TODO comments that should be resolved.

**Step 3: Squash or clean up commits if desired**

The feature branch should have clean, logical commits ready for merge or PR.
