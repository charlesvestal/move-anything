/*
 * uac2_daemon.c - FunctionFS UAC2 daemon for multichannel USB audio
 *
 * Presents a 10-channel USB Audio Class 2.0 input device to the host:
 *   Channels 1-2:  Slot 1 L/R (pre-volume)
 *   Channels 3-4:  Slot 2 L/R (pre-volume)
 *   Channels 5-6:  Slot 3 L/R (pre-volume)
 *   Channels 7-8:  Slot 4 L/R (pre-volume)
 *   Channels 9-10: Master Mix L/R (post-volume, pre-master-FX)
 *
 * Reads audio from the multichannel shared memory ring buffer written
 * by the shim's shadow_inprocess_render_to_buffer().
 *
 * Architecture:
 *   - Opens FunctionFS ep0, writes UAC2 descriptors
 *   - Opens isochronous IN endpoint for audio data
 *   - Opens shared memory ring buffer
 *   - Main loop: reads blocks from ring, writes ISO packets to host
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <endian.h>
#include <assert.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <poll.h>
#include <linux/usb/functionfs.h>

#include "uac2_descriptors.h"
#include "../host/shadow_constants.h"

/* ============================================================================
 * Daemon Configuration
 * ============================================================================ */

#define DEFAULT_FFS_PATH    "/dev/uac2_ffs"
#define PID_FILE            "/var/run/uac2_daemon.pid"
#define LOG_PREFIX          "uac2: "

/* USB frame timing */
#define USB_FRAME_US        1000    /* Full-speed: 1ms per frame */

/* Samples per USB frame at 44100 Hz (full-speed 1ms frames):
 * 44100 / 1000 = 44.1, so we alternate 44 and 45 samples.
 * Over 10 frames: 441 samples = exactly 10ms worth. */
#define SAMPLES_PER_FRAME_BASE  44
#define SAMPLES_PER_10_FRAMES   441

/* ============================================================================
 * UAC2 Descriptor Structures (10-channel production version)
 * ============================================================================ */

/* Reuse struct definitions from uac2_test.c but with 10 channels */

struct uac2_ac_header_desc {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bDescriptorSubtype;
    uint16_t bcdADC;
    uint8_t  bCategory;
    uint16_t wTotalLength;
    uint8_t  bmControls;
} __attribute__((packed));

struct uac2_clock_source_desc {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bDescriptorSubtype;
    uint8_t  bClockID;
    uint8_t  bmAttributes;
    uint8_t  bmControls;
    uint8_t  bAssocTerminal;
    uint8_t  iClockSource;
} __attribute__((packed));

struct uac2_input_terminal_desc {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bDescriptorSubtype;
    uint8_t  bTerminalID;
    uint16_t wTerminalType;
    uint8_t  bAssocTerminal;
    uint8_t  bCSourceID;
    uint8_t  bNrChannels;
    uint32_t bmChannelConfig;
    uint8_t  iChannelNames;
    uint16_t bmControls;
    uint8_t  iTerminal;
} __attribute__((packed));

struct uac2_output_terminal_desc {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bDescriptorSubtype;
    uint8_t  bTerminalID;
    uint16_t wTerminalType;
    uint8_t  bAssocTerminal;
    uint8_t  bSourceID;
    uint8_t  bCSourceID;
    uint16_t bmControls;
    uint8_t  iTerminal;
} __attribute__((packed));

struct uac2_as_general_desc {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bDescriptorSubtype;
    uint8_t  bTerminalLink;
    uint8_t  bmControls;
    uint8_t  bFormatType;
    uint32_t bmFormats;
    uint8_t  bNrChannels;
    uint32_t bmChannelConfig;
    uint8_t  iChannelNames;
} __attribute__((packed));

struct uac2_format_type_i_desc {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bDescriptorSubtype;
    uint8_t  bFormatType;
    uint8_t  bSubslotSize;
    uint8_t  bBitResolution;
} __attribute__((packed));

struct uac2_iso_endpoint_desc {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bDescriptorSubtype;
    uint8_t  bmAttributes;
    uint8_t  bmControls;
    uint8_t  bLockDelayUnits;
    uint16_t wLockDelay;
} __attribute__((packed));

/* usb_interface_descriptor comes from linux/usb/ch9.h
 * (included via linux/usb/functionfs.h).
 *
 * IMPORTANT: The kernel's struct usb_endpoint_descriptor is 9 bytes
 * (includes bRefresh + bSynchAddress audio extension fields), but
 * standard USB endpoint descriptors are 7 bytes (bLength=7).
 * FunctionFS parses descriptors by walking bLength offsets in the raw
 * byte blob, so using the kernel's 9-byte struct in a packed descriptor
 * set causes a 2-byte misalignment. We define a 7-byte version here. */
struct usb_ep_desc_std {
    uint8_t  bLength;           /* 7 */
    uint8_t  bDescriptorType;   /* 0x05 */
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} __attribute__((packed));

/* IAD is not in the kernel headers, define it here */
struct usb_iad_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bFirstInterface;
    uint8_t  bInterfaceCount;
    uint8_t  bFunctionClass;
    uint8_t  bFunctionSubClass;
    uint8_t  bFunctionProtocol;
    uint8_t  iFunction;
} __attribute__((packed));

/* Complete descriptor set */
struct uac2_full_descriptors {
    struct usb_iad_descriptor iad;
    struct usb_interface_descriptor ac_intf;
    struct uac2_ac_header_desc ac_header;
    struct uac2_clock_source_desc clock;
    struct uac2_input_terminal_desc input_term;
    struct uac2_output_terminal_desc output_term;
    struct usb_interface_descriptor as_intf_alt0;
    struct usb_interface_descriptor as_intf_alt1;
    struct uac2_as_general_desc as_general;
    struct uac2_format_type_i_desc format;
    struct usb_ep_desc_std ep_in;
    struct uac2_iso_endpoint_desc ep_cs;
} __attribute__((packed));

#define AC_HDR_TOTAL_LEN (sizeof(struct uac2_ac_header_desc) + \
                          sizeof(struct uac2_clock_source_desc) + \
                          sizeof(struct uac2_input_terminal_desc) + \
                          sizeof(struct uac2_output_terminal_desc))

/* Max packet: 45 samples × 10 channels × 2 bytes = 900 bytes */
#define MAX_PACKET_SIZE (45 * UAC2_NUM_CHANNELS * UAC2_BYTES_PER_SAMPLE)

/* Build UAC2 descriptors at runtime (htole16/htole32 are not constant expressions on glibc) */
static void build_descriptors(struct uac2_full_descriptors *d) {
    memset(d, 0, sizeof(*d));

    /* IAD */
    d->iad.bLength = 8;
    d->iad.bDescriptorType = 0x0B;
    d->iad.bFirstInterface = 0;
    d->iad.bInterfaceCount = 2;
    d->iad.bFunctionClass = USB_CLASS_AUDIO;
    d->iad.bFunctionSubClass = 0x00;
    d->iad.bFunctionProtocol = 0x20;
    d->iad.iFunction = UAC2_STR_ASSOC;

    /* AC interface */
    d->ac_intf.bLength = 9;
    d->ac_intf.bDescriptorType = 0x04;
    d->ac_intf.bInterfaceNumber = 0;
    d->ac_intf.bNumEndpoints = 0;
    d->ac_intf.bInterfaceClass = USB_CLASS_AUDIO;
    d->ac_intf.bInterfaceSubClass = USB_SUBCLASS_AUDIOCONTROL;
    d->ac_intf.bInterfaceProtocol = 0x20;
    d->ac_intf.iInterface = UAC2_STR_AC_IF;

    /* AC Header */
    d->ac_header.bLength = sizeof(struct uac2_ac_header_desc);
    d->ac_header.bDescriptorType = UAC2_CS_INTERFACE;
    d->ac_header.bDescriptorSubtype = UAC2_HEADER;
    d->ac_header.bcdADC = htole16(0x0200);
    d->ac_header.bCategory = 0x00;
    d->ac_header.wTotalLength = htole16(AC_HDR_TOTAL_LEN);
    d->ac_header.bmControls = 0x00;

    /* Clock Source */
    d->clock.bLength = sizeof(struct uac2_clock_source_desc);
    d->clock.bDescriptorType = UAC2_CS_INTERFACE;
    d->clock.bDescriptorSubtype = UAC2_CLOCK_SOURCE;
    d->clock.bClockID = UAC2_CLOCK_ID;
    d->clock.bmAttributes = UAC2_CLOCK_INTERNAL_FIXED;
    d->clock.bmControls = 0x01;
    d->clock.iClockSource = UAC2_STR_CLOCK;

    /* Input Terminal */
    d->input_term.bLength = sizeof(struct uac2_input_terminal_desc);
    d->input_term.bDescriptorType = UAC2_CS_INTERFACE;
    d->input_term.bDescriptorSubtype = UAC2_INPUT_TERMINAL;
    d->input_term.bTerminalID = UAC2_INPUT_TERMINAL_ID;
    d->input_term.wTerminalType = htole16(UAC2_INPUT_TERMINAL_UNDEFINED);
    d->input_term.bCSourceID = UAC2_CLOCK_ID;
    d->input_term.bNrChannels = UAC2_NUM_CHANNELS;
    d->input_term.bmChannelConfig = htole32(UAC2_CHANNEL_CONFIG);
    d->input_term.iTerminal = UAC2_STR_INPUT_TERM;

    /* Output Terminal */
    d->output_term.bLength = sizeof(struct uac2_output_terminal_desc);
    d->output_term.bDescriptorType = UAC2_CS_INTERFACE;
    d->output_term.bDescriptorSubtype = UAC2_OUTPUT_TERMINAL;
    d->output_term.bTerminalID = UAC2_OUTPUT_TERMINAL_ID;
    d->output_term.wTerminalType = htole16(UAC2_OUTPUT_TERMINAL_USB_STREAMING);
    d->output_term.bSourceID = UAC2_INPUT_TERMINAL_ID;
    d->output_term.bCSourceID = UAC2_CLOCK_ID;

    /* AS interface alt 0 (zero bandwidth) */
    d->as_intf_alt0.bLength = 9;
    d->as_intf_alt0.bDescriptorType = 0x04;
    d->as_intf_alt0.bInterfaceNumber = 1;
    d->as_intf_alt0.bAlternateSetting = 0;
    d->as_intf_alt0.bNumEndpoints = 0;
    d->as_intf_alt0.bInterfaceClass = USB_CLASS_AUDIO;
    d->as_intf_alt0.bInterfaceSubClass = USB_SUBCLASS_AUDIOSTREAMING;
    d->as_intf_alt0.bInterfaceProtocol = 0x20;
    d->as_intf_alt0.iInterface = UAC2_STR_AS_IF;

    /* AS interface alt 1 (active) */
    d->as_intf_alt1.bLength = 9;
    d->as_intf_alt1.bDescriptorType = 0x04;
    d->as_intf_alt1.bInterfaceNumber = 1;
    d->as_intf_alt1.bAlternateSetting = 1;
    d->as_intf_alt1.bNumEndpoints = 1;
    d->as_intf_alt1.bInterfaceClass = USB_CLASS_AUDIO;
    d->as_intf_alt1.bInterfaceSubClass = USB_SUBCLASS_AUDIOSTREAMING;
    d->as_intf_alt1.bInterfaceProtocol = 0x20;
    d->as_intf_alt1.iInterface = UAC2_STR_AS_IF;

    /* AS General */
    d->as_general.bLength = sizeof(struct uac2_as_general_desc);
    d->as_general.bDescriptorType = UAC2_CS_INTERFACE;
    d->as_general.bDescriptorSubtype = UAC2_AS_GENERAL;
    d->as_general.bTerminalLink = UAC2_OUTPUT_TERMINAL_ID;
    d->as_general.bFormatType = UAC2_FORMAT_TYPE_I;
    d->as_general.bmFormats = htole32(UAC2_PCM);
    d->as_general.bNrChannels = UAC2_NUM_CHANNELS;
    d->as_general.bmChannelConfig = htole32(UAC2_CHANNEL_CONFIG);

    /* Format Type I */
    d->format.bLength = sizeof(struct uac2_format_type_i_desc);
    d->format.bDescriptorType = UAC2_CS_INTERFACE;
    d->format.bDescriptorSubtype = UAC2_FORMAT_TYPE;
    d->format.bFormatType = UAC2_FORMAT_TYPE_I;
    d->format.bSubslotSize = UAC2_BYTES_PER_SAMPLE;
    d->format.bBitResolution = UAC2_BIT_DEPTH;

    /* ISO IN endpoint */
    d->ep_in.bLength = 7;
    d->ep_in.bDescriptorType = 0x05;
    d->ep_in.bEndpointAddress = 0x81;
    d->ep_in.bmAttributes = USB_ENDPOINT_XFER_ISOC | USB_ENDPOINT_SYNC_ASYNC;
    d->ep_in.wMaxPacketSize = htole16(MAX_PACKET_SIZE);
    d->ep_in.bInterval = 1;

    /* Class-specific ISO endpoint */
    d->ep_cs.bLength = sizeof(struct uac2_iso_endpoint_desc);
    d->ep_cs.bDescriptorType = UAC2_CS_ENDPOINT;
    d->ep_cs.bDescriptorSubtype = 0x00;
}

/* ============================================================================
 * FunctionFS Blob (built at runtime)
 * ============================================================================ */

struct ffs_desc_blob {
    struct usb_functionfs_descs_head_v2 header;
    __le32 fs_count;
    __le32 hs_count;
    struct uac2_full_descriptors fs_descs;
    struct uac2_full_descriptors hs_descs;
} __attribute__((packed));

#define FFS_DESC_COUNT 12  /* IAD + AC intf + 4 AC class + AS alt0 + AS alt1 + AS general + format + EP + EP_CS */

static void build_ffs_blob(struct ffs_desc_blob *blob) {
    memset(blob, 0, sizeof(*blob));
    blob->header.magic = htole32(FUNCTIONFS_DESCRIPTORS_MAGIC_V2);
    blob->header.flags = htole32(FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC);
    blob->header.length = htole32(sizeof(struct ffs_desc_blob));
    blob->fs_count = htole32(FFS_DESC_COUNT);
    blob->hs_count = htole32(FFS_DESC_COUNT);
    build_descriptors(&blob->fs_descs);
    build_descriptors(&blob->hs_descs);
}

#define FFS_STR_COUNT 5

/* ============================================================================
 * Global State
 * ============================================================================ */

static volatile int g_running = 1;
static volatile int g_streaming = 0;  /* Host has selected alt setting 1 */
static int g_ep0_fd = -1;
static int g_ep1_fd = -1;
static multichannel_shm_t *g_shm = NULL;

/* Sample accumulator for USB frame pacing */
static uint32_t g_frame_counter = 0;
static uint32_t g_last_read_seq = 0;

/* Residual sample buffer: holds leftover samples from a ring block
 * that didn't fit in the current USB frame */
static int16_t g_residual[128 * UAC2_NUM_CHANNELS]; /* Max one full block */
static int g_residual_frames = 0;  /* Number of frames remaining in residual */
static int g_residual_offset = 0;  /* Frame offset into residual */

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

/* ============================================================================
 * FunctionFS Setup
 * ============================================================================ */

static int write_ffs_descriptors(int ep0_fd) {
    struct ffs_desc_blob ffs_descs;
    build_ffs_blob(&ffs_descs);
    ssize_t ret = write(ep0_fd, &ffs_descs, sizeof(ffs_descs));
    if (ret < 0) {
        perror(LOG_PREFIX "write descriptors");
        return -1;
    }
    printf(LOG_PREFIX "wrote %zd bytes of descriptors\n", ret);

    /* Build string blob */
    const char *strings[] = {
        "Move Everything Audio",
        "Audio Control",
        "44100 Hz Internal Clock",
        "DSP Multichannel Source",
        "Audio Streaming",
    };

    size_t str_data_len = 0;
    for (int i = 0; i < FFS_STR_COUNT; i++)
        str_data_len += strlen(strings[i]) + 1;

    size_t blob_size = sizeof(struct usb_functionfs_strings_head) + 2 + str_data_len;
    uint8_t *blob = calloc(1, blob_size);
    if (!blob) return -1;

    struct usb_functionfs_strings_head *hdr = (void *)blob;
    hdr->magic = htole32(FUNCTIONFS_STRINGS_MAGIC);
    hdr->length = htole32(blob_size);
    hdr->str_count = htole32(FFS_STR_COUNT);
    hdr->lang_count = htole32(1);

    uint8_t *p = blob + sizeof(*hdr);
    *(uint16_t *)p = htole16(0x0409);
    p += 2;

    for (int i = 0; i < FFS_STR_COUNT; i++) {
        size_t len = strlen(strings[i]) + 1;
        memcpy(p, strings[i], len);
        p += len;
    }

    ret = write(ep0_fd, blob, blob_size);
    free(blob);
    if (ret < 0) {
        perror(LOG_PREFIX "write strings");
        return -1;
    }
    printf(LOG_PREFIX "wrote %zd bytes of strings\n", ret);
    return 0;
}

/* ============================================================================
 * Shared Memory
 * ============================================================================ */

static multichannel_shm_t *open_shm(void) {
    int fd = shm_open(SHM_SHADOW_MULTICHANNEL, O_RDWR, 0666);
    if (fd < 0) {
        perror(LOG_PREFIX "shm_open " SHM_SHADOW_MULTICHANNEL);
        return NULL;
    }

    void *ptr = mmap(NULL, MULTICHANNEL_SHM_SIZE,
                     PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    if (ptr == MAP_FAILED) {
        perror(LOG_PREFIX "mmap multichannel shm");
        return NULL;
    }

    multichannel_shm_t *shm = (multichannel_shm_t *)ptr;

    /* Validate header */
    if (shm->sample_rate != UAC2_SAMPLE_RATE ||
        shm->channels != UAC2_NUM_CHANNELS ||
        shm->frames_per_block != 128) {
        fprintf(stderr, LOG_PREFIX "shm header mismatch: rate=%u ch=%u fpb=%u\n",
                shm->sample_rate, shm->channels, shm->frames_per_block);
        munmap(ptr, MULTICHANNEL_SHM_SIZE);
        return NULL;
    }

    printf(LOG_PREFIX "shared memory opened: %u Hz, %u channels, %u frames/block, %u ring blocks\n",
           shm->sample_rate, shm->channels, shm->frames_per_block, shm->ring_blocks);
    return shm;
}

/* ============================================================================
 * Audio Streaming
 * ============================================================================ */

/*
 * Get the number of samples to send in this USB frame.
 * At 44100 Hz full-speed (1ms frames): 44.1 samples/frame.
 * We send 44 samples 9 times, then 45 once, per 10-frame cycle.
 * This gives exactly 441 samples per 10ms = 44100 samples/second.
 */
static int samples_for_frame(void) {
    int idx = g_frame_counter % 10;
    g_frame_counter++;
    /* Send 45 on frame 0, 44 on frames 1-9 */
    return (idx == 0) ? 45 : 44;
}

/*
 * Fill a USB frame packet from the ring buffer.
 * Returns number of bytes written to packet, or 0 on underrun.
 */
static int fill_usb_frame(int16_t *packet, int samples_needed) {
    int samples_filled = 0;

    /* First, drain any residual samples from previous block */
    while (g_residual_frames > 0 && samples_filled < samples_needed) {
        int src_off = g_residual_offset * UAC2_NUM_CHANNELS;
        int dst_off = samples_filled * UAC2_NUM_CHANNELS;
        memcpy(&packet[dst_off], &g_residual[src_off],
               UAC2_NUM_CHANNELS * sizeof(int16_t));
        samples_filled++;
        g_residual_offset++;
        g_residual_frames--;
    }

    /* Then read from ring buffer as needed */
    while (samples_filled < samples_needed && g_shm) {
        uint32_t wr = __sync_val_compare_and_swap(&g_shm->write_seq, 0, 0); /* Atomic read */
        if (g_last_read_seq >= wr) {
            /* No new data - underrun, fill remainder with silence */
            break;
        }

        uint32_t ring_idx = g_last_read_seq % g_shm->ring_blocks;
        int16_t *block = g_shm->ring +
                         (ring_idx * g_shm->frames_per_block * g_shm->channels);
        int block_frames = g_shm->frames_per_block;

        /* Copy frames from this block */
        int frames_to_copy = samples_needed - samples_filled;
        if (frames_to_copy > block_frames) frames_to_copy = block_frames;

        int dst_off = samples_filled * UAC2_NUM_CHANNELS;
        memcpy(&packet[dst_off], block,
               frames_to_copy * UAC2_NUM_CHANNELS * sizeof(int16_t));
        samples_filled += frames_to_copy;

        if (frames_to_copy < block_frames) {
            /* Save remaining frames as residual */
            int remaining = block_frames - frames_to_copy;
            memcpy(g_residual,
                   &block[frames_to_copy * UAC2_NUM_CHANNELS],
                   remaining * UAC2_NUM_CHANNELS * sizeof(int16_t));
            g_residual_frames = remaining;
            g_residual_offset = 0;
        }

        /* Advance read position */
        g_last_read_seq++;
        __sync_synchronize();
        g_shm->read_seq = g_last_read_seq;
    }

    /* Zero-fill any remaining samples (underrun → silence) */
    if (samples_filled < samples_needed) {
        int dst_off = samples_filled * UAC2_NUM_CHANNELS;
        int remaining = (samples_needed - samples_filled) * UAC2_NUM_CHANNELS;
        memset(&packet[dst_off], 0, remaining * sizeof(int16_t));
        samples_filled = samples_needed;
    }

    return samples_filled * UAC2_NUM_CHANNELS * sizeof(int16_t);
}

/* ============================================================================
 * ep0 Event Handling
 * ============================================================================ */

static void handle_ep0_events(void) {
    struct usb_functionfs_event event;
    ssize_t ret = read(g_ep0_fd, &event, sizeof(event));
    if (ret < (ssize_t)sizeof(event)) return;

    switch (event.type) {
    case FUNCTIONFS_BIND:
        printf(LOG_PREFIX "gadget bound\n");
        break;
    case FUNCTIONFS_UNBIND:
        printf(LOG_PREFIX "gadget unbound\n");
        g_streaming = 0;
        break;
    case FUNCTIONFS_ENABLE:
        printf(LOG_PREFIX "function enabled (host selected alt 1)\n");
        g_streaming = 1;
        /* Sync to current write position to avoid playing old data */
        if (g_shm) {
            g_last_read_seq = g_shm->write_seq;
            g_shm->read_seq = g_last_read_seq;
        }
        g_residual_frames = 0;
        g_frame_counter = 0;
        break;
    case FUNCTIONFS_DISABLE:
        printf(LOG_PREFIX "function disabled (host selected alt 0)\n");
        g_streaming = 0;
        break;
    case FUNCTIONFS_SETUP: {
        /* Handle class-specific requests (e.g., clock frequency queries) */
        struct usb_ctrlrequest *ctrl = &event.u.setup;
        uint8_t req_type = ctrl->bRequestType;
        uint8_t req = ctrl->bRequest;
        uint16_t w_value = le16toh(ctrl->wValue);
        uint16_t w_index = le16toh(ctrl->wIndex);
        uint16_t w_length = le16toh(ctrl->wLength);

        printf(LOG_PREFIX "SETUP: type=0x%02x req=0x%02x val=0x%04x idx=0x%04x len=%u\n",
               req_type, req, w_value, w_index, w_length);

        /* CUR request for clock frequency */
        if (req == 0x01 && /* CUR */
            (w_value >> 8) == 0x01 && /* SAM_FREQ_CONTROL */
            (w_index >> 8) == UAC2_CLOCK_ID) {
            uint32_t freq = htole32(UAC2_SAMPLE_RATE);
            write(g_ep0_fd, &freq, sizeof(freq));
        } else if (req == 0x02 && /* RANGE */
                   (w_value >> 8) == 0x01 &&
                   (w_index >> 8) == UAC2_CLOCK_ID) {
            /* Clock frequency range: one triplet (min, max, res) */
            struct {
                uint16_t wNumSubRanges;
                uint32_t dMIN;
                uint32_t dMAX;
                uint32_t dRES;
            } __attribute__((packed)) range = {
                .wNumSubRanges = htole16(1),
                .dMIN = htole32(UAC2_SAMPLE_RATE),
                .dMAX = htole32(UAC2_SAMPLE_RATE),
                .dRES = htole32(0),
            };
            write(g_ep0_fd, &range, sizeof(range));
        } else {
            /* Stall unsupported requests */
            if (ctrl->bRequestType & 0x80) {
                /* IN request - respond with zero-length */
                uint8_t dummy = 0;
                write(g_ep0_fd, &dummy, 0);
            } else {
                /* OUT request - read and discard */
                if (w_length > 0) {
                    uint8_t buf[256];
                    read(g_ep0_fd, buf, w_length < sizeof(buf) ? w_length : sizeof(buf));
                }
            }
        }
        break;
    }
    case FUNCTIONFS_SUSPEND:
        printf(LOG_PREFIX "USB suspend\n");
        g_streaming = 0;
        break;
    case FUNCTIONFS_RESUME:
        printf(LOG_PREFIX "USB resume\n");
        break;
    default:
        printf(LOG_PREFIX "unknown event type %d\n", event.type);
        break;
    }
}

/* ============================================================================
 * PID File Management
 * ============================================================================ */

static void write_pid_file(void) {
    FILE *f = fopen(PID_FILE, "w");
    if (f) {
        fprintf(f, "%d\n", getpid());
        fclose(f);
    }
}

static void remove_pid_file(void) {
    unlink(PID_FILE);
}

/* ============================================================================
 * Main
 * ============================================================================ */

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s [-f ffs_path] [-d] [-h]\n", prog);
    fprintf(stderr, "  -f PATH   FunctionFS mount point (default: %s)\n", DEFAULT_FFS_PATH);
    fprintf(stderr, "  -d        Run as daemon (background)\n");
    fprintf(stderr, "  -h        Show this help\n");
}

int main(int argc, char *argv[]) {
    const char *ffs_path = DEFAULT_FFS_PATH;
    int daemonize = 0;

    int opt;
    while ((opt = getopt(argc, argv, "f:dh")) != -1) {
        switch (opt) {
        case 'f': ffs_path = optarg; break;
        case 'd': daemonize = 1; break;
        case 'h':
        default:
            print_usage(argv[0]);
            return (opt == 'h') ? 0 : 1;
        }
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    if (daemonize) {
        if (daemon(0, 1) < 0) {  /* Keep stdout/stderr for logging */
            perror(LOG_PREFIX "daemon");
            return 1;
        }
    }

    write_pid_file();

    /* Open ep0 */
    char path[256];
    snprintf(path, sizeof(path), "%s/ep0", ffs_path);
    printf(LOG_PREFIX "opening %s\n", path);
    g_ep0_fd = open(path, O_RDWR);
    if (g_ep0_fd < 0) {
        perror(LOG_PREFIX "open ep0");
        remove_pid_file();
        return 1;
    }

    /* Write descriptors */
    if (write_ffs_descriptors(g_ep0_fd) < 0) {
        close(g_ep0_fd);
        remove_pid_file();
        return 1;
    }
    printf(LOG_PREFIX "descriptors written, waiting for gadget bind\n");

    /* Open ep1 (iso IN) */
    snprintf(path, sizeof(path), "%s/ep1", ffs_path);
    g_ep1_fd = open(path, O_WRONLY);
    if (g_ep1_fd < 0) {
        perror(LOG_PREFIX "open ep1");
        close(g_ep0_fd);
        remove_pid_file();
        return 1;
    }
    printf(LOG_PREFIX "ep1 opened for isochronous streaming\n");

    /* Open shared memory (retry loop - shim may not have started yet) */
    for (int retry = 0; retry < 30 && g_running; retry++) {
        g_shm = open_shm();
        if (g_shm) break;
        printf(LOG_PREFIX "waiting for shared memory (attempt %d/30)...\n", retry + 1);
        sleep(1);
    }

    if (!g_shm) {
        fprintf(stderr, LOG_PREFIX "could not open shared memory after 30 attempts\n");
        close(g_ep1_fd);
        close(g_ep0_fd);
        remove_pid_file();
        return 1;
    }

    /* Initialize read position to current write position */
    g_last_read_seq = g_shm->write_seq;
    g_shm->read_seq = g_last_read_seq;

    printf(LOG_PREFIX "streaming loop started (10-channel, 44100 Hz)\n");

    /* Packet buffer for one USB frame */
    int16_t packet[45 * UAC2_NUM_CHANNELS]; /* Max 45 samples × 10 channels */

    /* Main loop */
    struct pollfd pfd = { .fd = g_ep0_fd, .events = POLLIN };

    while (g_running) {
        /* Check for ep0 events (non-blocking) */
        int poll_ret = poll(&pfd, 1, 0);
        if (poll_ret > 0 && (pfd.revents & POLLIN)) {
            handle_ep0_events();
        }

        if (!g_streaming) {
            /* Not streaming - sleep and poll for events */
            usleep(10000); /* 10ms */
            continue;
        }

        /* Determine samples for this frame */
        int samples = samples_for_frame();
        int bytes = fill_usb_frame(packet, samples);

        /* Write to isochronous endpoint */
        ssize_t written = write(g_ep1_fd, packet, bytes);
        if (written < 0) {
            if (errno == ESHUTDOWN || errno == ECONNRESET) {
                printf(LOG_PREFIX "USB disconnected\n");
                g_streaming = 0;
                continue;
            }
            if (errno == EAGAIN) {
                /* Buffer full, skip this frame */
                continue;
            }
            perror(LOG_PREFIX "write ep1");
            break;
        }

        /* Pace to ~1ms per frame (USB scheduling provides real timing,
         * but we avoid busy-waiting) */
        usleep(900);
    }

    printf(LOG_PREFIX "shutting down\n");

    if (g_shm) {
        munmap(g_shm, MULTICHANNEL_SHM_SIZE);
    }
    if (g_ep1_fd >= 0) close(g_ep1_fd);
    if (g_ep0_fd >= 0) close(g_ep0_fd);
    remove_pid_file();

    return 0;
}
