/*
 * move-inject - Inject MIDI events into Move via the shim's shared memory ring.
 *
 * Usage:
 *   move-inject note-on  <note> <velocity>   # Note On, channel 1, cable 0
 *   move-inject note-off <note>              # Note Off, channel 1, cable 0
 *   move-inject cc       <cc>   <value>      # Control Change, channel 1, cable 0
 *   move-inject raw      <byte1> <byte2> <byte3>  # Raw (CIN auto-derived from status)
 *
 * Requires move-anything to be running (shim creates /dev/shm/move-inject-midi).
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../host/shadow_constants.h"

static void usage(void)
{
    fprintf(stderr,
        "Usage:\n"
        "  move-inject note-on  <note 0-127> <velocity 0-127>\n"
        "  move-inject note-off <note 0-127>\n"
        "  move-inject cc       <cc 0-127>   <value 0-127>\n"
        "  move-inject raw      <byte1>      <byte2>      <byte3>\n"
        "\nRequires move-anything to be running.\n");
}

/* Derive CIN from MIDI status byte (cable 0, standard messages only). */
static uint8_t cin_from_status(uint8_t status)
{
    switch (status & 0xF0) {
        case 0x80: return 0x08;  /* Note Off */
        case 0x90: return 0x09;  /* Note On */
        case 0xA0: return 0x0A;  /* Poly KeyPress */
        case 0xB0: return 0x0B;  /* Control Change */
        case 0xC0: return 0x0C;  /* Program Change */
        case 0xD0: return 0x0D;  /* Channel Pressure */
        case 0xE0: return 0x0E;  /* Pitch Bend */
        default:   return 0x0F;  /* Single byte / unknown */
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage();
        return 1;
    }

    uint8_t pkt[4] = {0, 0, 0, 0};

    if (strcmp(argv[1], "note-on") == 0) {
        if (argc != 4) { usage(); return 1; }
        uint8_t note = (uint8_t)atoi(argv[2]);
        uint8_t vel  = (uint8_t)atoi(argv[3]);
        pkt[0] = 0x09;   /* CIN=9, cable 0 */
        pkt[1] = 0x90;   /* Note On, channel 1 */
        pkt[2] = note;
        pkt[3] = vel;
    } else if (strcmp(argv[1], "note-off") == 0) {
        if (argc != 3) { usage(); return 1; }
        uint8_t note = (uint8_t)atoi(argv[2]);
        pkt[0] = 0x08;   /* CIN=8, cable 0 */
        pkt[1] = 0x80;   /* Note Off, channel 1 */
        pkt[2] = note;
        pkt[3] = 0x00;
    } else if (strcmp(argv[1], "cc") == 0) {
        if (argc != 4) { usage(); return 1; }
        uint8_t cc  = (uint8_t)atoi(argv[2]);
        uint8_t val = (uint8_t)atoi(argv[3]);
        pkt[0] = 0x0B;   /* CIN=B, cable 0 */
        pkt[1] = 0xB0;   /* Control Change, channel 1 */
        pkt[2] = cc;
        pkt[3] = val;
    } else if (strcmp(argv[1], "raw") == 0) {
        if (argc != 5) { usage(); return 1; }
        uint8_t b1 = (uint8_t)atoi(argv[2]);
        uint8_t b2 = (uint8_t)atoi(argv[3]);
        uint8_t b3 = (uint8_t)atoi(argv[4]);
        pkt[0] = cin_from_status(b1);  /* CIN derived from status, cable 0 */
        pkt[1] = b1;
        pkt[2] = b2;
        pkt[3] = b3;
    } else {
        fprintf(stderr, "Unknown command: %s\n", argv[1]);
        usage();
        return 1;
    }

    /* Open the inject shm (must already exist — shim creates it on startup) */
    int fd = shm_open(SHM_INJECT_MIDI, O_RDWR, 0);
    if (fd < 0) {
        perror("shm_open (is move-anything running?)");
        return 1;
    }

    inject_midi_t *shm = (inject_midi_t *)mmap(NULL, INJECT_MIDI_BUFFER_SIZE,
                                                PROT_READ | PROT_WRITE,
                                                MAP_SHARED, fd, 0);
    close(fd);

    if (shm == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    /* Check ring is not full */
    uint8_t widx = shm->write_idx;
    uint8_t ridx = shm->read_idx;
    if ((uint8_t)(widx - ridx) >= INJECT_MIDI_MAX_PACKETS) {
        fprintf(stderr, "move-inject: ring buffer full — shim may not be running\n");
        munmap(shm, INJECT_MIDI_BUFFER_SIZE);
        return 1;
    }

    /* Write packet and advance write index */
    uint8_t *slot = shm->buffer + ((widx % INJECT_MIDI_MAX_PACKETS) * 4);
    slot[0] = pkt[0];
    slot[1] = pkt[1];
    slot[2] = pkt[2];
    slot[3] = pkt[3];
    __sync_synchronize();
    shm->write_idx = (uint8_t)(widx + 1);

    munmap(shm, INJECT_MIDI_BUFFER_SIZE);
    return 0;
}
