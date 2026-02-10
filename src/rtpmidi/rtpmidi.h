/*
 * rtpmidi.h - RTP-MIDI daemon types and constants
 *
 * Implements Apple's RTP-MIDI (RFC 6295 / AppleMIDI) protocol for
 * wireless MIDI input to Move Anything's shadow instrument.
 */

#ifndef RTPMIDI_H
#define RTPMIDI_H

#include <stdint.h>
#include <netinet/in.h>
#include <sys/socket.h>

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
    struct sockaddr_storage remote_addr;
    socklen_t remote_addr_len;
} rtpmidi_session_t;

/* USB-MIDI CIN values */
static inline uint8_t midi_status_to_cin(uint8_t status) {
    return (status >> 4) & 0x0F;
}

/* Format a MIDI message as a cable-2 USB-MIDI packet (external MIDI) */
static inline void format_usb_midi_packet(uint8_t *out, uint8_t status, uint8_t d1, uint8_t d2) {
    uint8_t cin = midi_status_to_cin(status);
    out[0] = 0x20 | cin;  /* Cable 2 + CIN */
    out[1] = status;
    out[2] = d1;
    out[3] = d2;
}

#endif /* RTPMIDI_H */
