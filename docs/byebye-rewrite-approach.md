# ByeBye Rewrite Approach — Fallback for Quantum-Free Playback

If the tempo-dodge approach doesn't work (Move resets session tempo before
`requestBeatAtTime`), this approach rewrites the subscriber's ALIVE packets
to ByeBye in the shim's `recvfrom` hook. numPeers drops to 0, but audio
keeps flowing because base Link ByeBye does NOT trigger LinkAudio cleanup.

## Why It Works

The Link SDK has two separate protocol layers:

1. **Discovery** (multicast 224.76.78.75:20808): ALIVE/Response/ByeBye
   - Manages peer list, numPeers count
   - ByeBye removes peer from base Link peer list

2. **LinkAudio** (unicast UDP, chnnlsv protocol): Session/Request/Audio/ChannelByes
   - Manages audio channel subscriptions, mPeerSendHandlers
   - Only `kChannelByes` (type 2 in chnnlsv) triggers `pruneSendHandlers()`

A base Link ByeBye does NOT trigger `kChannelByes`. So:
- numPeers drops to 0 (no quantum on Play)
- mPeerSendHandlers persists (audio keeps flowing)
- Subscriber's chnnlsv unicast traffic is NOT filtered (only discovery multicast)

## Discovery Protocol Wire Format

```
Offset  Size  Field
0       7     Magic: "_asdp_v" (0x5F 61 73 64 70 5F 76)
7       1     Version: 0x01
8       1     Message Type: 1=ALIVE, 2=Response, 3=ByeBye
9       1     TTL
10      2     Session Group ID (always 0)
12      16    Node ID (peer identifier)
28+     var   Payload (endpoint data for ALIVE/Response, empty for ByeBye)
```

To convert ALIVE → ByeBye: change byte 8 from 0x01 to 0x03. The SDK's
ByeBye handler only reads the header (type + nodeId), ignoring extra payload.

## Implementation Plan

### 1. Add recvfrom/recvmsg hooks to shim

```c
/* In the hook/interposition section of move_anything_shim.c */

static ssize_t (*real_recvfrom)(int, void*, size_t, int,
                                 struct sockaddr*, socklen_t*) = NULL;

ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
                 struct sockaddr *src_addr, socklen_t *addrlen)
{
    if (!real_recvfrom)
        real_recvfrom = dlsym(RTLD_NEXT, "recvfrom");

    ssize_t n = real_recvfrom(sockfd, buf, len, flags, src_addr, addrlen);

    /* Only process if Link Audio is enabled and we're filtering */
    if (n >= 28 && link_audio.enabled && link_audio.filter_subscriber_alive) {
        uint8_t *pkt = (uint8_t *)buf;

        /* Check discovery protocol magic: "_asdp_v\x01" */
        if (memcmp(pkt, "_asdp_v\x01", 8) == 0) {
            uint8_t msg_type = pkt[8];

            /* Check if this is an ALIVE (type 1) from our subscriber */
            if (msg_type == 1 && link_audio.subscriber_node_id_known) {
                if (memcmp(pkt + 12, link_audio.subscriber_node_id, 16) == 0) {
                    /* Rewrite ALIVE → ByeBye (type 3) */
                    pkt[8] = 3;
                }
            }

            /* Learn subscriber's node ID from ALIVE packets containing "ME-Sub" */
            if (msg_type == 1 && !link_audio.subscriber_node_id_known && n > 40) {
                /* Search for "ME-Sub" peer name in payload */
                for (ssize_t i = 28; i < n - 6; i++) {
                    if (memcmp(pkt + i, "ME-Sub", 6) == 0) {
                        memcpy(link_audio.subscriber_node_id, pkt + 12, 16);
                        link_audio.subscriber_node_id_known = 1;
                        shadow_log("Link Audio: learned subscriber nodeId");
                        break;
                    }
                }
            }
        }
    }

    return n;
}
```

### 2. Add fields to link_audio_state_t (link_audio.h)

```c
/* ByeBye rewrite state for quantum-free playback */
volatile int filter_subscriber_alive;     /* 1 = rewrite ALIVE→ByeBye */
uint8_t subscriber_node_id[16];           /* subscriber's 16-byte nodeId */
volatile int subscriber_node_id_known;    /* 1 = nodeId captured */
```

### 3. Transport signaling in shim (replaces quantum-dodge)

```c
/* On MIDI Stop (0xFC): start filtering subscriber's ALIVE */
static void link_audio_start_alive_filter(void)
{
    if (!link_audio.enabled) return;
    link_audio.filter_subscriber_alive = 1;
    shadow_log("Link Audio: ALIVE filter ON (transport stopped)");
}

/* On MIDI Start (0xFA/0xFB): stop filtering */
static void link_audio_stop_alive_filter(void)
{
    if (!link_audio.enabled) return;
    link_audio.filter_subscriber_alive = 0;
    shadow_log("Link Audio: ALIVE filter OFF (transport started)");
}
```

In the MIDI_OUT processing:
```c
if (status_usb == 0xFC) {
    link_audio_start_alive_filter();
} else if (status_usb == 0xFA || status_usb == 0xFB) {
    link_audio_stop_alive_filter();
}
```

### 4. Subscriber changes

Subscriber becomes much simpler — no flag files needed at all for transport:
- Always enabled, always active
- No quantum-dodge or tempo manipulation
- Just subscribes to channels and stays alive

The only remaining flag file is the tempo file (for initial tempo at startup).

### 5. Boot behavior

At boot:
- Subscriber starts, enables Link, discovers channels, audio flows
- Shim sets `filter_subscriber_alive = 1` at startup (in `load_feature_config`)
- First subscriber ALIVE that Move receives gets rewritten to ByeBye
- numPeers stays 0 from Move's perspective
- First Play starts instantly

### 6. Real peer detection

The recvfrom hook only rewrites packets matching the subscriber's nodeId.
Other peers (Live) are unaffected. When Live connects:
- Live's ALIVE passes through unmodified → Move sees numPeers >= 1
- Quantum sync works normally with Live
- Subscriber's ALIVE still gets rewritten → doesn't add to numPeers

This means quantum is always dodged regardless of Live being connected.
If we want to preserve quantum with Live, we'd need the shim to detect
Live's presence (e.g., numPeers > expected) and stop filtering. But this
is complex — the shim doesn't have direct access to numPeers.

Alternative: subscriber could write numPeers to a file periodically,
and shim reads it to decide whether to filter. But this adds latency.

Simplest: always filter subscriber's ALIVE. If Live is connected, Move
sees numPeers = (total peers - 1) since subscriber is hidden. With just
Live + Move, numPeers = 1 on Move's side, quantum still works. With
Live + Move + subscriber (filtered), Move sees numPeers = 1 (just Live).
This is actually perfect — quantum sync with Live, no subscriber noise.

### 7. Timing considerations

- Between Stop and Play: subscriber's ALIVE arrives every ~1s. At least
  one gets rewritten to ByeBye before user presses Play (unless < 1s).
- After ByeBye: Move's Link SDK processes it → peer removed → numPeers drops.
  This is near-instant once the ByeBye is received.
- On Play: filter is cleared. Next subscriber ALIVE restores numPeers.
  This happens after requestBeatAtTime, so doesn't affect quantum.

### 8. Key SDK references

- `libs/link/include/ableton/discovery/PeerGateway.hpp` — ByeBye processing
- `libs/link/include/ableton/link_audio/UdpMessenger.hpp:477` — mReceivers check
- `libs/link/include/ableton/link_audio/Channels.hpp:354-370` — pruneSendHandlers
- `libs/link/include/ableton/discovery/IpInterface.hpp:30-39` — multicast endpoints
- Discovery message types: kAlive=1, kResponse=2, kByeBye=3 (byte offset 8)
- NodeId: 16 bytes at offset 12 in discovery packets
