/*
 * uac2_test.c - FunctionFS isochronous endpoint validation test
 *
 * Phase 0: Minimal test to verify that FunctionFS isochronous IN endpoints
 * work on Move's DWC2 USB controller. Writes a 440Hz sine wave as a
 * stereo USB audio device (simpler than full 10-channel for validation).
 *
 * Usage:
 *   1. Set up FunctionFS gadget (see setup_gadget_test.sh)
 *   2. Run: ./uac2_test /dev/uac2_ffs
 *   3. Check host: system_profiler SPAudioDataType (macOS)
 *   4. Record in Audacity/QuickTime to verify sine wave
 *
 * This is a throwaway validation program - not production code.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <endian.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/usb/functionfs.h>

#include "uac2_descriptors.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Test config: stereo only for validation */
#define TEST_CHANNELS   2
#define TEST_SAMPLE_RATE 44100
#define TEST_FREQ       440.0   /* Hz - A4 */
#define TEST_AMPLITUDE  16000   /* ~50% of int16 range */

/* Full-speed USB: 1ms frames, ~44 samples per frame at 44100 Hz */
#define SAMPLES_PER_FRAME   44
#define FRAME_EXTRA_SAMPLE  1   /* Every ~10th frame needs 45 samples */
#define BYTES_PER_FRAME_44  (44 * TEST_CHANNELS * 2)
#define BYTES_PER_FRAME_45  (45 * TEST_CHANNELS * 2)

static volatile int running = 1;

static void signal_handler(int sig) {
    (void)sig;
    running = 0;
}

/* ============================================================================
 * UAC2 Descriptors for FunctionFS (stereo test version)
 *
 * The descriptor blob is written to ep0 to configure FunctionFS.
 * Structure: FunctionFS descriptor header, then FS descriptors, then HS.
 * ============================================================================ */

/* AudioControl Header */
struct uac2_ac_header_desc {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bDescriptorSubtype;
    uint16_t bcdADC;
    uint8_t  bCategory;
    uint16_t wTotalLength;
    uint8_t  bmControls;
} __attribute__((packed));

/* Clock Source */
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

/* Input Terminal */
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

/* Output Terminal */
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

/* AS General */
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

/* Format Type I */
struct uac2_format_type_i_desc {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bDescriptorSubtype;
    uint8_t  bFormatType;
    uint8_t  bSubslotSize;
    uint8_t  bBitResolution;
} __attribute__((packed));

/* Class-specific isochronous endpoint */
struct uac2_iso_endpoint_desc {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bDescriptorSubtype;
    uint8_t  bmAttributes;
    uint8_t  bmControls;
    uint8_t  bLockDelayUnits;
    uint16_t wLockDelay;
} __attribute__((packed));

/*
 * Complete descriptor set for FunctionFS.
 * We provide both full-speed and high-speed (identical for this test).
 */

/* usb_interface_descriptor comes from linux/usb/ch9.h.
 *
 * IMPORTANT: The kernel's struct usb_endpoint_descriptor is 9 bytes
 * (includes bRefresh + bSynchAddress), but standard endpoint descriptors
 * are 7 bytes. FunctionFS parses by bLength offsets, so we need a 7-byte
 * struct to avoid misalignment in the packed descriptor blob. */
struct usb_ep_desc_std {
    uint8_t  bLength;           /* 7 */
    uint8_t  bDescriptorType;   /* 0x05 */
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} __attribute__((packed));

/* IAD not in kernel headers */
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

/*
 * The full descriptor blob for one speed configuration.
 * FunctionFS needs: IAD + AC interface + AC class descriptors +
 *                   AS interface alt0 + AS interface alt1 + AS class + endpoint
 */
struct uac2_descriptors {
    /* Interface Association */
    struct usb_iad_descriptor iad;
    /* AudioControl interface */
    struct usb_interface_descriptor ac_intf;
    struct uac2_ac_header_desc ac_header;
    struct uac2_clock_source_desc clock;
    struct uac2_input_terminal_desc input_term;
    struct uac2_output_terminal_desc output_term;
    /* AudioStreaming interface - alt 0 (zero bandwidth) */
    struct usb_interface_descriptor as_intf_alt0;
    /* AudioStreaming interface - alt 1 (active) */
    struct usb_interface_descriptor as_intf_alt1;
    struct uac2_as_general_desc as_general;
    struct uac2_format_type_i_desc format;
    struct usb_ep_desc_std ep_in;
    struct uac2_iso_endpoint_desc ep_cs;
} __attribute__((packed));

/* AC header total length: header + clock + input term + output term */
#define AC_TOTAL_LEN (sizeof(struct uac2_ac_header_desc) + \
                      sizeof(struct uac2_clock_source_desc) + \
                      sizeof(struct uac2_input_terminal_desc) + \
                      sizeof(struct uac2_output_terminal_desc))

/* Build test descriptors at runtime (htole16/htole32 are not constant expressions on glibc) */
static void build_test_descriptors(struct uac2_descriptors *d) {
    memset(d, 0, sizeof(*d));

    d->iad.bLength = 8;
    d->iad.bDescriptorType = 0x0B;
    d->iad.bInterfaceCount = 2;
    d->iad.bFunctionClass = USB_CLASS_AUDIO;
    d->iad.bFunctionProtocol = 0x20;
    d->iad.iFunction = UAC2_STR_ASSOC;

    d->ac_intf.bLength = 9;
    d->ac_intf.bDescriptorType = 0x04;
    d->ac_intf.bInterfaceClass = USB_CLASS_AUDIO;
    d->ac_intf.bInterfaceSubClass = USB_SUBCLASS_AUDIOCONTROL;
    d->ac_intf.bInterfaceProtocol = 0x20;
    d->ac_intf.iInterface = UAC2_STR_AC_IF;

    d->ac_header.bLength = sizeof(struct uac2_ac_header_desc);
    d->ac_header.bDescriptorType = UAC2_CS_INTERFACE;
    d->ac_header.bDescriptorSubtype = UAC2_HEADER;
    d->ac_header.bcdADC = htole16(0x0200);
    d->ac_header.wTotalLength = htole16(AC_TOTAL_LEN);

    d->clock.bLength = sizeof(struct uac2_clock_source_desc);
    d->clock.bDescriptorType = UAC2_CS_INTERFACE;
    d->clock.bDescriptorSubtype = UAC2_CLOCK_SOURCE;
    d->clock.bClockID = UAC2_CLOCK_ID;
    d->clock.bmAttributes = UAC2_CLOCK_INTERNAL_FIXED;
    d->clock.bmControls = 0x01;
    d->clock.iClockSource = UAC2_STR_CLOCK;

    d->input_term.bLength = sizeof(struct uac2_input_terminal_desc);
    d->input_term.bDescriptorType = UAC2_CS_INTERFACE;
    d->input_term.bDescriptorSubtype = UAC2_INPUT_TERMINAL;
    d->input_term.bTerminalID = UAC2_INPUT_TERMINAL_ID;
    d->input_term.wTerminalType = htole16(UAC2_INPUT_TERMINAL_UNDEFINED);
    d->input_term.bCSourceID = UAC2_CLOCK_ID;
    d->input_term.bNrChannels = TEST_CHANNELS;
    d->input_term.bmChannelConfig = htole32(0x00000003);
    d->input_term.iTerminal = UAC2_STR_INPUT_TERM;

    d->output_term.bLength = sizeof(struct uac2_output_terminal_desc);
    d->output_term.bDescriptorType = UAC2_CS_INTERFACE;
    d->output_term.bDescriptorSubtype = UAC2_OUTPUT_TERMINAL;
    d->output_term.bTerminalID = UAC2_OUTPUT_TERMINAL_ID;
    d->output_term.wTerminalType = htole16(UAC2_OUTPUT_TERMINAL_USB_STREAMING);
    d->output_term.bSourceID = UAC2_INPUT_TERMINAL_ID;
    d->output_term.bCSourceID = UAC2_CLOCK_ID;

    d->as_intf_alt0.bLength = 9;
    d->as_intf_alt0.bDescriptorType = 0x04;
    d->as_intf_alt0.bInterfaceNumber = 1;
    d->as_intf_alt0.bInterfaceClass = USB_CLASS_AUDIO;
    d->as_intf_alt0.bInterfaceSubClass = USB_SUBCLASS_AUDIOSTREAMING;
    d->as_intf_alt0.bInterfaceProtocol = 0x20;
    d->as_intf_alt0.iInterface = UAC2_STR_AS_IF;

    d->as_intf_alt1.bLength = 9;
    d->as_intf_alt1.bDescriptorType = 0x04;
    d->as_intf_alt1.bInterfaceNumber = 1;
    d->as_intf_alt1.bAlternateSetting = 1;
    d->as_intf_alt1.bNumEndpoints = 1;
    d->as_intf_alt1.bInterfaceClass = USB_CLASS_AUDIO;
    d->as_intf_alt1.bInterfaceSubClass = USB_SUBCLASS_AUDIOSTREAMING;
    d->as_intf_alt1.bInterfaceProtocol = 0x20;
    d->as_intf_alt1.iInterface = UAC2_STR_AS_IF;

    d->as_general.bLength = sizeof(struct uac2_as_general_desc);
    d->as_general.bDescriptorType = UAC2_CS_INTERFACE;
    d->as_general.bDescriptorSubtype = UAC2_AS_GENERAL;
    d->as_general.bTerminalLink = UAC2_OUTPUT_TERMINAL_ID;
    d->as_general.bFormatType = UAC2_FORMAT_TYPE_I;
    d->as_general.bmFormats = htole32(UAC2_PCM);
    d->as_general.bNrChannels = TEST_CHANNELS;
    d->as_general.bmChannelConfig = htole32(0x00000003);

    d->format.bLength = sizeof(struct uac2_format_type_i_desc);
    d->format.bDescriptorType = UAC2_CS_INTERFACE;
    d->format.bDescriptorSubtype = UAC2_FORMAT_TYPE;
    d->format.bFormatType = UAC2_FORMAT_TYPE_I;
    d->format.bSubslotSize = 2;
    d->format.bBitResolution = 16;

    d->ep_in.bLength = 7;
    d->ep_in.bDescriptorType = 0x05;
    d->ep_in.bEndpointAddress = 0x81;
    d->ep_in.bmAttributes = USB_ENDPOINT_XFER_ISOC | USB_ENDPOINT_SYNC_ASYNC;
    d->ep_in.wMaxPacketSize = htole16(45 * TEST_CHANNELS * 2);
    d->ep_in.bInterval = 1;

    d->ep_cs.bLength = sizeof(struct uac2_iso_endpoint_desc);
    d->ep_cs.bDescriptorType = UAC2_CS_ENDPOINT;
}

/* FunctionFS descriptor blob */

struct ffs_descs_blob {
    struct usb_functionfs_descs_head_v2 header;
    __le32 fs_count;
    __le32 hs_count;
    struct uac2_descriptors fs_descs;
    struct uac2_descriptors hs_descs;
} __attribute__((packed));

#define TEST_DESC_COUNT 12

#define UAC2_STR_COUNT 5

static int write_descriptors(int ep0_fd) {
    /* Build and write descriptor blob */
    struct ffs_descs_blob ffs_descriptors;
    memset(&ffs_descriptors, 0, sizeof(ffs_descriptors));
    ffs_descriptors.header.magic = htole32(FUNCTIONFS_DESCRIPTORS_MAGIC_V2);
    ffs_descriptors.header.flags = htole32(FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC);
    ffs_descriptors.header.length = htole32(sizeof(struct ffs_descs_blob));
    ffs_descriptors.fs_count = htole32(TEST_DESC_COUNT);
    ffs_descriptors.hs_count = htole32(TEST_DESC_COUNT);
    build_test_descriptors(&ffs_descriptors.fs_descs);
    build_test_descriptors(&ffs_descriptors.hs_descs);

    ssize_t ret = write(ep0_fd, &ffs_descriptors, sizeof(ffs_descriptors));
    if (ret < 0) {
        perror("uac2_test: write descriptors");
        return -1;
    }
    printf("uac2_test: wrote %zd bytes of descriptors\n", ret);

    /* Build and write string blob */
    const char *strings[] = {
        "Move Everything Audio",     /* STR_ASSOC */
        "Audio Control",             /* STR_AC_IF */
        "44100 Hz Internal Clock",   /* STR_CLOCK */
        "DSP Audio Source",          /* STR_INPUT_TERM */
        "Audio Streaming",           /* STR_AS_IF */
    };

    /* Calculate total string blob size */
    size_t str_data_len = 0;
    for (int i = 0; i < UAC2_STR_COUNT; i++) {
        str_data_len += strlen(strings[i]) + 1;
    }

    size_t blob_size = sizeof(struct usb_functionfs_strings_head) + 2 + str_data_len;
    uint8_t *blob = calloc(1, blob_size);
    if (!blob) {
        perror("uac2_test: calloc strings");
        return -1;
    }

    struct usb_functionfs_strings_head *hdr = (void *)blob;
    hdr->magic = htole32(FUNCTIONFS_STRINGS_MAGIC);
    hdr->length = htole32(blob_size);
    hdr->str_count = htole32(UAC2_STR_COUNT);
    hdr->lang_count = htole32(1);

    uint8_t *p = blob + sizeof(*hdr);
    *(uint16_t *)p = htole16(0x0409); /* English */
    p += 2;

    for (int i = 0; i < UAC2_STR_COUNT; i++) {
        size_t len = strlen(strings[i]) + 1;
        memcpy(p, strings[i], len);
        p += len;
    }

    ret = write(ep0_fd, blob, blob_size);
    free(blob);
    if (ret < 0) {
        perror("uac2_test: write strings");
        return -1;
    }
    printf("uac2_test: wrote %zd bytes of strings\n", ret);
    return 0;
}

/* Generate 440Hz sine wave samples */
static void generate_sine(int16_t *buf, int num_samples, double *phase) {
    double phase_inc = 2.0 * M_PI * TEST_FREQ / TEST_SAMPLE_RATE;
    for (int i = 0; i < num_samples; i++) {
        int16_t sample = (int16_t)(TEST_AMPLITUDE * sin(*phase));
        buf[i * 2]     = htole16(sample);  /* Left */
        buf[i * 2 + 1] = htole16(sample);  /* Right */
        *phase += phase_inc;
        if (*phase >= 2.0 * M_PI) *phase -= 2.0 * M_PI;
    }
}

int main(int argc, char *argv[]) {
    const char *ffs_path = "/dev/uac2_ffs";
    if (argc > 1) ffs_path = argv[1];

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Open ep0 for descriptors and control */
    char ep0_path[256];
    snprintf(ep0_path, sizeof(ep0_path), "%s/ep0", ffs_path);

    printf("uac2_test: opening %s\n", ep0_path);
    int ep0_fd = open(ep0_path, O_RDWR);
    if (ep0_fd < 0) {
        perror("uac2_test: open ep0");
        fprintf(stderr, "Make sure FunctionFS is mounted at %s\n", ffs_path);
        return 1;
    }

    /* Write descriptors to ep0 */
    if (write_descriptors(ep0_fd) < 0) {
        close(ep0_fd);
        return 1;
    }

    printf("uac2_test: descriptors written. Waiting for USB bind...\n");
    printf("uac2_test: now bind the gadget: echo 'fe980000.usb' > /sys/kernel/config/usb_gadget/g1/UDC\n");

    /* Open ep1 for isochronous data */
    char ep1_path[256];
    snprintf(ep1_path, sizeof(ep1_path), "%s/ep1", ffs_path);

    int ep1_fd = open(ep1_path, O_WRONLY);
    if (ep1_fd < 0) {
        perror("uac2_test: open ep1");
        fprintf(stderr, "Could not open iso endpoint. Is the gadget bound?\n");
        close(ep0_fd);
        return 1;
    }

    printf("uac2_test: ep1 opened. Streaming 440Hz sine wave...\n");
    printf("uac2_test: check host with: system_profiler SPAudioDataType\n");
    printf("uac2_test: press Ctrl+C to stop\n");

    /* Main streaming loop */
    double phase = 0.0;
    int frame_counter = 0;
    int16_t audio_buf[45 * TEST_CHANNELS]; /* Max samples per frame */

    while (running) {
        /* At 44100 Hz, full-speed USB 1ms frames need 44 or 45 samples.
         * Pattern: 44,45,44,45,44,45,44,45,44,45 per 10 frames = 445 samples = ~10.09ms
         * Simpler: alternate 44/45 to approximate. Exact: 44100/1000 = 44.1 */
        int samples = (frame_counter % 10 == 0) ? 45 : 44;
        frame_counter++;

        generate_sine(audio_buf, samples, &phase);

        int bytes = samples * TEST_CHANNELS * 2;
        ssize_t written = write(ep1_fd, audio_buf, bytes);
        if (written < 0) {
            if (errno == ESHUTDOWN || errno == ECONNRESET) {
                printf("uac2_test: USB disconnected, waiting...\n");
                usleep(100000);
                continue;
            }
            perror("uac2_test: write ep1");
            break;
        }

        /* Pace ourselves roughly to 1ms per frame.
         * The kernel's USB scheduling provides actual timing,
         * but we don't want to spin-loop filling the buffer. */
        usleep(900); /* Slightly less than 1ms to keep ahead */
    }

    printf("uac2_test: shutting down\n");
    close(ep1_fd);
    close(ep0_fd);
    return 0;
}
