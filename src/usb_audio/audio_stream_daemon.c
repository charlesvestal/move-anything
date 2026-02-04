/*
 * audio_stream_daemon.c - UDP audio streaming daemon for multichannel audio
 *
 * Reads multichannel audio from the shared memory ring buffer
 * and streams it over UDP to the host Mac via the NCM USB link.
 *
 * Channel layout (14 channels, interleaved int16):
 *   Channels  1-2:  Slot 1 L/R (pre-volume)
 *   Channels  3-4:  Slot 2 L/R (pre-volume)
 *   Channels  5-6:  Slot 3 L/R (pre-volume)
 *   Channels  7-8:  Slot 4 L/R (pre-volume)
 *   Channels  9-10: ME Stereo Mix L/R (post-volume, pre-master-FX)
 *   Channels 11-12: Move Native L/R (without Move Everything)
 *   Channels 13-14: Combined L/R (Move + ME, post-master-FX)
 *
 * Protocol: UDP broadcast to 172.16.254.255:4010
 * Packet: 16-byte header + 2560 bytes PCM = 2576 bytes
 *
 * Usage:
 *   audio_stream_daemon        # Run in foreground
 *   audio_stream_daemon -d     # Daemonize
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "../host/shadow_constants.h"

/* ============================================================================
 * Configuration
 * ============================================================================ */

#define BROADCAST_ADDR  "172.16.254.255"
#define BROADCAST_PORT  4010
#define PID_FILE        "/var/run/audio_stream_daemon.pid"
#define LOG_PREFIX      "audio_stream: "

/* Poll interval when no new data (microseconds) */
#define POLL_INTERVAL_US  500

/* ============================================================================
 * UDP Packet Format
 * ============================================================================ */

#define AUDIO_PACKET_MAGIC  0x4D564155  /* 'MVAU' */

typedef struct {
    uint32_t magic;
    uint32_t sequence;
    uint16_t channels;
    uint16_t frames;
    uint16_t sample_rate;
    uint16_t bits_per_sample;
} __attribute__((packed)) audio_packet_header_t;

/* PCM payload size: 128 frames * 10 channels * 2 bytes = 2560 */
#define PCM_PAYLOAD_SIZE \
    (MULTICHANNEL_FRAMES_PER_BLOCK * MULTICHANNEL_NUM_CHANNELS * sizeof(int16_t))

#define PACKET_SIZE (sizeof(audio_packet_header_t) + PCM_PAYLOAD_SIZE)

/* ============================================================================
 * Globals
 * ============================================================================ */

static volatile int g_running = 1;
static multichannel_shm_t *g_shm = NULL;
static int g_sock = -1;

/* ============================================================================
 * Signal Handling
 * ============================================================================ */

static void signal_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

static void install_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
}

/* ============================================================================
 * PID File
 * ============================================================================ */

static void write_pid_file(void)
{
    FILE *f = fopen(PID_FILE, "w");
    if (f) {
        fprintf(f, "%d\n", getpid());
        fclose(f);
    }
}

static void remove_pid_file(void)
{
    unlink(PID_FILE);
}

/* ============================================================================
 * Shared Memory
 * ============================================================================ */

static multichannel_shm_t *open_shm(void)
{
    int fd = shm_open(SHM_SHADOW_MULTICHANNEL, O_RDONLY, 0);
    if (fd < 0) {
        fprintf(stderr, LOG_PREFIX "shm_open(%s) failed: %s\n",
                SHM_SHADOW_MULTICHANNEL, strerror(errno));
        return NULL;
    }

    void *ptr = mmap(NULL, MULTICHANNEL_SHM_SIZE, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);

    if (ptr == MAP_FAILED) {
        fprintf(stderr, LOG_PREFIX "mmap failed: %s\n", strerror(errno));
        return NULL;
    }

    multichannel_shm_t *shm = (multichannel_shm_t *)ptr;

    /* Validate header */
    if (shm->channels != MULTICHANNEL_NUM_CHANNELS ||
        shm->frames_per_block != MULTICHANNEL_FRAMES_PER_BLOCK ||
        shm->ring_blocks != MULTICHANNEL_RING_BLOCKS) {
        fprintf(stderr, LOG_PREFIX "shm header mismatch: ch=%u fpb=%u rb=%u\n",
                shm->channels, shm->frames_per_block, shm->ring_blocks);
        munmap(ptr, MULTICHANNEL_SHM_SIZE);
        return NULL;
    }

    fprintf(stderr, LOG_PREFIX "opened shm: %u ch, %u frames/block, %u ring blocks, sr=%u\n",
            shm->channels, shm->frames_per_block, shm->ring_blocks, shm->sample_rate);

    return shm;
}

/* ============================================================================
 * UDP Socket
 * ============================================================================ */

static int open_udp_socket(struct sockaddr_in *dest_addr)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        fprintf(stderr, LOG_PREFIX "socket() failed: %s\n", strerror(errno));
        return -1;
    }

    /* Enable broadcast */
    int broadcast = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast)) < 0) {
        fprintf(stderr, LOG_PREFIX "SO_BROADCAST failed: %s\n", strerror(errno));
        close(sock);
        return -1;
    }

    memset(dest_addr, 0, sizeof(*dest_addr));
    dest_addr->sin_family = AF_INET;
    dest_addr->sin_port = htons(BROADCAST_PORT);
    dest_addr->sin_addr.s_addr = inet_addr(BROADCAST_ADDR);

    return sock;
}

/* ============================================================================
 * Main Loop
 * ============================================================================ */

static void stream_loop(void)
{
    struct sockaddr_in dest_addr;
    uint8_t packet[PACKET_SIZE];
    audio_packet_header_t *hdr = (audio_packet_header_t *)packet;
    int16_t *pcm = (int16_t *)(packet + sizeof(audio_packet_header_t));

    g_sock = open_udp_socket(&dest_addr);
    if (g_sock < 0)
        return;

    /* Prepare static header fields */
    hdr->magic = AUDIO_PACKET_MAGIC;
    hdr->channels = MULTICHANNEL_NUM_CHANNELS;
    hdr->frames = MULTICHANNEL_FRAMES_PER_BLOCK;
    hdr->sample_rate = 44100;
    hdr->bits_per_sample = 16;

    uint32_t last_seq = g_shm->write_seq;
    uint32_t packet_seq = 0;
    uint32_t underruns = 0;
    uint32_t blocks_sent = 0;

    fprintf(stderr, LOG_PREFIX "streaming to %s:%d (starting at seq %u)\n",
            BROADCAST_ADDR, BROADCAST_PORT, last_seq);

    while (g_running) {
        uint32_t write_seq = g_shm->write_seq;

        if (write_seq == last_seq) {
            /* No new data - sleep briefly */
            usleep(POLL_INTERVAL_US);
            continue;
        }

        /* Check for overrun (we fell behind) */
        uint32_t available = write_seq - last_seq;
        if (available > g_shm->ring_blocks) {
            /* We're too far behind - skip to latest */
            underruns++;
            last_seq = write_seq - 1;
            available = 1;
        }

        /* Send all available blocks */
        while (last_seq < write_seq && g_running) {
            uint32_t ring_idx = last_seq % g_shm->ring_blocks;
            size_t offset = ring_idx * g_shm->frames_per_block * g_shm->channels;
            const int16_t *src = &g_shm->ring[offset];

            /* Copy PCM data into packet */
            memcpy(pcm, src, PCM_PAYLOAD_SIZE);

            /* Set sequence number */
            hdr->sequence = packet_seq++;

            /* Send */
            ssize_t sent = sendto(g_sock, packet, PACKET_SIZE, 0,
                                  (struct sockaddr *)&dest_addr, sizeof(dest_addr));
            if (sent < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    fprintf(stderr, LOG_PREFIX "sendto failed: %s\n", strerror(errno));
                }
            }

            last_seq++;
            blocks_sent++;

            /* Periodic status */
            if ((blocks_sent % 10000) == 0) {
                fprintf(stderr, LOG_PREFIX "sent %u blocks, %u underruns\n",
                        blocks_sent, underruns);
            }
        }
    }

    fprintf(stderr, LOG_PREFIX "stopping (sent %u blocks, %u underruns)\n",
            blocks_sent, underruns);
    close(g_sock);
    g_sock = -1;
}

/* ============================================================================
 * Daemonize
 * ============================================================================ */

static void daemonize(void)
{
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(1);
    }
    if (pid > 0)
        exit(0);  /* Parent exits */

    setsid();

    /* Redirect stdio to /dev/null */
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        /* Keep stderr for logging (redirected by caller if needed) */
        close(devnull);
    }
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(int argc, char *argv[])
{
    int daemon_mode = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0)
            daemon_mode = 1;
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            fprintf(stderr, "Usage: %s [-d]\n"
                    "  -d  Daemonize (run in background)\n", argv[0]);
            return 0;
        }
    }

    install_signal_handlers();

    /* Wait for shared memory to become available */
    fprintf(stderr, LOG_PREFIX "waiting for shared memory...\n");
    while (g_running) {
        g_shm = open_shm();
        if (g_shm)
            break;
        sleep(2);
    }

    if (!g_running || !g_shm)
        return 1;

    if (daemon_mode)
        daemonize();

    write_pid_file();

    stream_loop();

    /* Cleanup */
    if (g_shm) {
        munmap((void *)g_shm, MULTICHANNEL_SHM_SIZE);
        g_shm = NULL;
    }
    remove_pid_file();

    return 0;
}
