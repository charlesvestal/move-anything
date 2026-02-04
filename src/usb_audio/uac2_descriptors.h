/*
 * uac2_descriptors.h - USB Audio Class 2.0 descriptor definitions
 *
 * FunctionFS UAC2 descriptors for a 10-channel (5 stereo pairs) USB audio
 * input device at 44100 Hz, 16-bit PCM.
 *
 * Channel layout:
 *   1-2:  Slot 1 L/R
 *   3-4:  Slot 2 L/R
 *   5-6:  Slot 3 L/R
 *   7-8:  Slot 4 L/R
 *   9-10: Master Mix L/R
 *
 * Note: All multi-byte values in USB descriptors are little-endian.
 * The target platform (ARM) is also little-endian, so no conversion needed
 * in static initializers.
 */

#ifndef UAC2_DESCRIPTORS_H
#define UAC2_DESCRIPTORS_H

#include <stdint.h>

/* ============================================================================
 * USB Audio Class 2.0 Constants
 * Only define if not already provided by linux/usb/ch9.h
 * ============================================================================ */

#ifndef USB_CLASS_AUDIO
#define USB_CLASS_AUDIO         0x01
#endif
#define USB_SUBCLASS_AUDIOCONTROL   0x01
#define USB_SUBCLASS_AUDIOSTREAMING 0x02

/* Audio class-specific descriptor types */
#define UAC2_CS_INTERFACE       0x24
#define UAC2_CS_ENDPOINT        0x25

/* AudioControl interface descriptor subtypes */
#define UAC2_HEADER             0x01
#define UAC2_INPUT_TERMINAL     0x02
#define UAC2_OUTPUT_TERMINAL    0x03
#define UAC2_CLOCK_SOURCE       0x0A

/* AudioStreaming interface descriptor subtypes */
#define UAC2_AS_GENERAL         0x01
#define UAC2_FORMAT_TYPE        0x02

/* Terminal types */
#define UAC2_INPUT_TERMINAL_UNDEFINED   0x0200
#define UAC2_OUTPUT_TERMINAL_USB_STREAMING 0x0101

/* Clock attributes */
#define UAC2_CLOCK_INTERNAL_FIXED   0x01

/* Format type codes */
#define UAC2_FORMAT_TYPE_I      0x01

/* Audio data format - PCM */
#define UAC2_PCM                0x00000001

/* Endpoint attributes for isochronous */
#ifndef USB_ENDPOINT_XFER_ISOC
#define USB_ENDPOINT_XFER_ISOC  0x01
#endif
#ifndef USB_ENDPOINT_SYNC_ASYNC
#define USB_ENDPOINT_SYNC_ASYNC 0x04  /* Asynchronous */
#endif

/* ============================================================================
 * Device Configuration
 * ============================================================================ */

#define UAC2_NUM_CHANNELS       10
#define UAC2_SAMPLE_RATE        44100
#define UAC2_BIT_DEPTH          16
#define UAC2_BYTES_PER_SAMPLE   2
#define UAC2_NUM_SLOTS          4

/* Isochronous packet sizing for high-speed USB (125us microframes)
 * At 44100 Hz high-speed: ~5.5 samples per microframe
 * We need ceil(44100/8000) = 6 samples max per microframe
 * Max packet = 6 samples * 10 channels * 2 bytes = 120 bytes
 * For full-speed (1ms frames): 45 samples * 10ch * 2 = 900 bytes
 */
#define UAC2_MAX_PACKET_SIZE_FS 900   /* Full-speed: 44 or 45 samples per frame */
#define UAC2_MAX_PACKET_SIZE_HS 120   /* High-speed: 5 or 6 samples per microframe */

/* Use full-speed for DWC2 gadget (Move's USB is full-speed to host) */
#define UAC2_MAX_PACKET_SIZE    UAC2_MAX_PACKET_SIZE_FS

/* ============================================================================
 * Clock Source ID and Terminal IDs
 * ============================================================================ */

#define UAC2_CLOCK_ID           0x01
#define UAC2_INPUT_TERMINAL_ID  0x02
#define UAC2_OUTPUT_TERMINAL_ID 0x03

/* ============================================================================
 * Channel configuration for 10 channels
 * No predefined spatial positions - just raw channels.
 * ============================================================================ */

#define UAC2_CHANNEL_CONFIG     0x00000000  /* Non-predefined spatial */

/* ============================================================================
 * FunctionFS string IDs (match indices in string descriptors)
 * ============================================================================ */

#define UAC2_STR_NONE           0
#define UAC2_STR_ASSOC          1
#define UAC2_STR_AC_IF          2
#define UAC2_STR_CLOCK          3
#define UAC2_STR_INPUT_TERM     4
#define UAC2_STR_AS_IF          5

#endif /* UAC2_DESCRIPTORS_H */
