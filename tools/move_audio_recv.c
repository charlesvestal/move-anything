/*
 * move_audio_recv.c - Receive multichannel audio from Move via UDP
 *
 * Receives 14-channel audio streamed by audio_stream_daemon on Move
 * and outputs it via CoreAudio to BlackHole-16ch (or another device),
 * or records to WAV files. Resamples automatically if device rate differs.
 *
 * Channel layout:
 *   Channels  1-2:  Slot 1 L/R (pre-volume)
 *   Channels  3-4:  Slot 2 L/R (pre-volume)
 *   Channels  5-6:  Slot 3 L/R (pre-volume)
 *   Channels  7-8:  Slot 4 L/R (pre-volume)
 *   Channels  9-10: ME Stereo Mix L/R (post-volume, pre-master-FX)
 *   Channels 11-12: Move Native L/R (without Move Everything)
 *   Channels 13-14: Combined L/R (Move + ME, post-master-FX)
 *
 * Usage:
 *   move_audio_recv                          # Stream to BlackHole 16ch
 *   move_audio_recv --device "My Device"     # Stream to named device
 *   move_audio_recv --list-devices           # List audio output devices
 *   move_audio_recv --wav session.wav        # Record to WAV file
 *   move_audio_recv --wav session.wav --split # Record split per-slot WAVs
 *   move_audio_recv --duration 10            # Record for 10 seconds
 *
 * Build:
 *   cc -O2 -o move_audio_recv tools/move_audio_recv.c \
 *      -framework CoreAudio -framework AudioToolbox -framework CoreFoundation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

#include <CoreAudio/CoreAudio.h>
#include <AudioToolbox/AudioToolbox.h>

/* ============================================================================
 * Constants (must match daemon)
 * ============================================================================ */

#define AUDIO_PACKET_MAGIC  0x4D564155  /* 'MVAU' */
#define UDP_PORT            4010
#define NUM_CHANNELS        14
#define FRAMES_PER_BLOCK    128
#define SAMPLE_RATE         44100
#define BITS_PER_SAMPLE     16

#define DEFAULT_DEVICE_NAME "BlackHole 16ch"

/* Ring buffer: hold enough blocks to absorb jitter */
#define RING_BLOCKS         64

/* ============================================================================
 * Packet Format
 * ============================================================================ */

typedef struct {
    uint32_t magic;
    uint32_t sequence;
    uint16_t channels;
    uint16_t frames;
    uint16_t sample_rate;
    uint16_t bits_per_sample;
} __attribute__((packed)) audio_packet_header_t;

#define PCM_PAYLOAD_SIZE    (FRAMES_PER_BLOCK * NUM_CHANNELS * sizeof(int16_t))
#define PACKET_SIZE         (sizeof(audio_packet_header_t) + PCM_PAYLOAD_SIZE)

/* ============================================================================
 * Lock-free Ring Buffer
 * ============================================================================ */

typedef struct {
    int16_t data[RING_BLOCKS][FRAMES_PER_BLOCK * NUM_CHANNELS];
    volatile uint32_t write_pos;    /* Next position to write */
    volatile uint32_t read_pos;     /* Next position to read */
} ring_buffer_t;

static ring_buffer_t g_ring;

static inline uint32_t ring_available(void)
{
    return g_ring.write_pos - g_ring.read_pos;
}

static inline bool ring_full(void)
{
    return ring_available() >= RING_BLOCKS;
}

static inline void ring_write(const int16_t *data)
{
    uint32_t idx = g_ring.write_pos % RING_BLOCKS;
    memcpy(g_ring.data[idx], data, PCM_PAYLOAD_SIZE);
    __sync_synchronize();
    g_ring.write_pos++;
}

static inline const int16_t *ring_read(void)
{
    if (ring_available() == 0)
        return NULL;
    uint32_t idx = g_ring.read_pos % RING_BLOCKS;
    const int16_t *ptr = g_ring.data[idx];
    __sync_synchronize();
    g_ring.read_pos++;
    return ptr;
}

/* ============================================================================
 * Globals
 * ============================================================================ */

static volatile int g_running = 1;
static volatile bool g_connected = false;
static volatile uint32_t g_packets_received = 0;
static volatile uint32_t g_packets_dropped = 0;
static volatile uint32_t g_last_sequence = 0;
static volatile uint32_t g_underruns = 0;

/* ============================================================================
 * Signal Handling
 * ============================================================================ */

static void signal_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

/* ============================================================================
 * WAV File Writing
 * ============================================================================ */

typedef struct {
    char riff[4];           /* "RIFF" */
    uint32_t file_size;     /* File size - 8 */
    char wave[4];           /* "WAVE" */
    char fmt[4];            /* "fmt " */
    uint32_t fmt_size;      /* 16 */
    uint16_t format;        /* 1 = PCM */
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char data[4];           /* "data" */
    uint32_t data_size;
} __attribute__((packed)) wav_header_t;

static void wav_write_header(FILE *f, uint16_t channels, uint32_t sample_rate,
                             uint16_t bits, uint32_t data_bytes)
{
    wav_header_t hdr;
    memcpy(hdr.riff, "RIFF", 4);
    hdr.file_size = 36 + data_bytes;
    memcpy(hdr.wave, "WAVE", 4);
    memcpy(hdr.fmt, "fmt ", 4);
    hdr.fmt_size = 16;
    hdr.format = 1;
    hdr.channels = channels;
    hdr.sample_rate = sample_rate;
    hdr.byte_rate = sample_rate * channels * (bits / 8);
    hdr.block_align = channels * (bits / 8);
    hdr.bits_per_sample = bits;
    memcpy(hdr.data, "data", 4);
    hdr.data_size = data_bytes;
    fwrite(&hdr, sizeof(hdr), 1, f);
}

static void wav_update_sizes(FILE *f, uint32_t data_bytes)
{
    /* Update RIFF size */
    uint32_t riff_size = 36 + data_bytes;
    fseek(f, 4, SEEK_SET);
    fwrite(&riff_size, 4, 1, f);

    /* Update data size */
    fseek(f, 40, SEEK_SET);
    fwrite(&data_bytes, 4, 1, f);

    fseek(f, 0, SEEK_END);
}

/* ============================================================================
 * CoreAudio Device Discovery
 * ============================================================================ */

static AudioDeviceID find_device_by_name(const char *name)
{
    AudioObjectPropertyAddress prop = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    UInt32 size = 0;
    OSStatus err = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &prop, 0, NULL, &size);
    if (err != noErr)
        return kAudioObjectUnknown;

    int count = (int)(size / sizeof(AudioDeviceID));
    AudioDeviceID *devices = malloc(size);
    if (!devices)
        return kAudioObjectUnknown;

    err = AudioObjectGetPropertyData(kAudioObjectSystemObject, &prop, 0, NULL, &size, devices);
    if (err != noErr) {
        free(devices);
        return kAudioObjectUnknown;
    }

    AudioDeviceID found = kAudioObjectUnknown;
    for (int i = 0; i < count; i++) {
        CFStringRef cfname = NULL;
        UInt32 nameSize = sizeof(cfname);
        AudioObjectPropertyAddress nameProp = {
            kAudioDevicePropertyDeviceNameCFString,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        err = AudioObjectGetPropertyData(devices[i], &nameProp, 0, NULL, &nameSize, &cfname);
        if (err != noErr || !cfname)
            continue;

        char buf[256];
        if (CFStringGetCString(cfname, buf, sizeof(buf), kCFStringEncodingUTF8)) {
            if (strcmp(buf, name) == 0) {
                found = devices[i];
                CFRelease(cfname);
                break;
            }
        }
        CFRelease(cfname);
    }

    free(devices);
    return found;
}

static void list_output_devices(void)
{
    AudioObjectPropertyAddress prop = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    UInt32 size = 0;
    OSStatus err = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &prop, 0, NULL, &size);
    if (err != noErr) {
        fprintf(stderr, "Failed to get device list\n");
        return;
    }

    int count = (int)(size / sizeof(AudioDeviceID));
    AudioDeviceID *devices = malloc(size);
    if (!devices)
        return;

    err = AudioObjectGetPropertyData(kAudioObjectSystemObject, &prop, 0, NULL, &size, devices);
    if (err != noErr) {
        free(devices);
        return;
    }

    printf("Audio output devices:\n");
    for (int i = 0; i < count; i++) {
        /* Check if device has output channels */
        AudioObjectPropertyAddress outProp = {
            kAudioDevicePropertyStreamConfiguration,
            kAudioDevicePropertyScopeOutput,
            kAudioObjectPropertyElementMain
        };
        UInt32 outSize = 0;
        err = AudioObjectGetPropertyDataSize(devices[i], &outProp, 0, NULL, &outSize);
        if (err != noErr)
            continue;

        AudioBufferList *bufs = malloc(outSize);
        if (!bufs)
            continue;
        err = AudioObjectGetPropertyData(devices[i], &outProp, 0, NULL, &outSize, bufs);
        if (err != noErr) {
            free(bufs);
            continue;
        }

        int total_ch = 0;
        for (UInt32 b = 0; b < bufs->mNumberBuffers; b++)
            total_ch += (int)bufs->mBuffers[b].mNumberChannels;
        free(bufs);

        if (total_ch == 0)
            continue;

        CFStringRef cfname = NULL;
        UInt32 nameSize = sizeof(cfname);
        AudioObjectPropertyAddress nameProp = {
            kAudioDevicePropertyDeviceNameCFString,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        err = AudioObjectGetPropertyData(devices[i], &nameProp, 0, NULL, &nameSize, &cfname);
        if (err != noErr)
            continue;

        /* Get sample rate */
        Float64 dev_sr = 0;
        UInt32 srSize = sizeof(dev_sr);
        AudioObjectPropertyAddress srProp = {
            kAudioDevicePropertyNominalSampleRate,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        AudioObjectGetPropertyData(devices[i], &srProp, 0, NULL, &srSize, &dev_sr);

        char buf[256];
        if (CFStringGetCString(cfname, buf, sizeof(buf), kCFStringEncodingUTF8))
            printf("  %-40s (%d ch, %.0f Hz)\n", buf, total_ch, dev_sr);
        CFRelease(cfname);
    }

    free(devices);
}

/* ============================================================================
 * Resampler (linear interpolation, 44100 -> device rate)
 * ============================================================================ */

/* Input accumulation buffer - holds int16 samples consumed from ring.
 * Needs to hold enough for one CoreAudio callback worth of input plus
 * one extra frame for interpolation. 2048 frames handles up to ~47ms. */
#define RESAMPLE_BUF_FRAMES 2048

static struct {
    double ratio;       /* source_rate / dest_rate (e.g. 44100/48000 = 0.919) */
    double phase;       /* Fractional position in input buffer */
    int16_t buf[RESAMPLE_BUF_FRAMES * NUM_CHANNELS];
    uint32_t buf_frames;
    bool active;        /* false = passthrough (rates match) */
} g_resampler;

static void resampler_init(double source_rate, double dest_rate)
{
    g_resampler.ratio = source_rate / dest_rate;
    g_resampler.phase = 0.0;
    g_resampler.buf_frames = 0;
    g_resampler.active = ((int)source_rate != (int)dest_rate);
}

/* Feed a 128-frame block into the resampler's input buffer.
 * Returns false if buffer is full. */
static bool resampler_feed(const int16_t *block)
{
    if (g_resampler.buf_frames + FRAMES_PER_BLOCK > RESAMPLE_BUF_FRAMES)
        return false;
    memcpy(&g_resampler.buf[g_resampler.buf_frames * NUM_CHANNELS],
           block, FRAMES_PER_BLOCK * NUM_CHANNELS * sizeof(int16_t));
    g_resampler.buf_frames += FRAMES_PER_BLOCK;
    return true;
}

/* Compact the resampler buffer by removing consumed frames */
static void resampler_compact(void)
{
    uint32_t consumed = (uint32_t)g_resampler.phase;
    if (consumed == 0 || consumed >= g_resampler.buf_frames)
        return;
    uint32_t remaining = g_resampler.buf_frames - consumed;
    memmove(g_resampler.buf,
            &g_resampler.buf[consumed * NUM_CHANNELS],
            remaining * NUM_CHANNELS * sizeof(int16_t));
    g_resampler.buf_frames = remaining;
    g_resampler.phase -= consumed;
}

/* ============================================================================
 * CoreAudio Output
 * ============================================================================ */

static AudioDeviceID g_device = kAudioObjectUnknown;
static AudioDeviceIOProcID g_io_proc_id = NULL;

/* Target ring buffer fill level in blocks - low for minimal latency,
 * high enough to absorb jitter */
#define TARGET_RING_FILL  4

static OSStatus audio_io_proc(AudioDeviceID device,
                              const AudioTimeStamp *now,
                              const AudioBufferList *inputData,
                              const AudioTimeStamp *inputTime,
                              AudioBufferList *outputData,
                              const AudioTimeStamp *outputTime,
                              void *clientData)
{
    (void)device; (void)now; (void)inputData; (void)inputTime;
    (void)outputTime; (void)clientData;

    /* Clear all output buffers first */
    for (UInt32 buf = 0; buf < outputData->mNumberBuffers; buf++) {
        memset(outputData->mBuffers[buf].mData, 0,
               outputData->mBuffers[buf].mDataByteSize);
    }

    for (UInt32 buf = 0; buf < outputData->mNumberBuffers; buf++) {
        float *out = (float *)outputData->mBuffers[buf].mData;
        UInt32 out_channels = outputData->mBuffers[buf].mNumberChannels;
        UInt32 out_frames = outputData->mBuffers[buf].mDataByteSize /
                            (out_channels * sizeof(float));
        UInt32 ch_to_copy = out_channels < NUM_CHANNELS ? out_channels : NUM_CHANNELS;

        if (!g_resampler.active) {
            /* Passthrough: rates match, copy blocks directly */
            UInt32 frames_written = 0;
            while (frames_written < out_frames) {
                const int16_t *block = ring_read();
                if (!block) {
                    g_underruns++;
                    break;
                }

                UInt32 frames_to_copy = FRAMES_PER_BLOCK;
                if (frames_written + frames_to_copy > out_frames)
                    frames_to_copy = out_frames - frames_written;

                for (UInt32 f = 0; f < frames_to_copy; f++) {
                    for (UInt32 c = 0; c < ch_to_copy; c++) {
                        out[(frames_written + f) * out_channels + c] =
                            (float)block[f * NUM_CHANNELS + c] / 32768.0f;
                    }
                }
                frames_written += frames_to_copy;
            }
        } else {
            /* Resampling: fill input buffer, then interpolate to output */

            /* Calculate how many input frames we need:
             * out_frames * ratio + 1 (for interpolation lookahead) */
            uint32_t input_needed = (uint32_t)(out_frames * g_resampler.ratio) + 2;
            uint32_t phase_floor = (uint32_t)g_resampler.phase;
            input_needed += phase_floor;

            /* Feed blocks until we have enough input */
            while (g_resampler.buf_frames < input_needed) {
                const int16_t *block = ring_read();
                if (!block) {
                    g_underruns++;
                    break;
                }
                if (!resampler_feed(block))
                    break;
            }

            /* Linear interpolation */
            for (UInt32 f = 0; f < out_frames; f++) {
                uint32_t idx = (uint32_t)g_resampler.phase;
                double frac = g_resampler.phase - idx;

                if (idx + 1 >= g_resampler.buf_frames)
                    break;  /* Not enough input - rest stays silent */

                const int16_t *s0 = &g_resampler.buf[idx * NUM_CHANNELS];
                const int16_t *s1 = &g_resampler.buf[(idx + 1) * NUM_CHANNELS];

                for (UInt32 c = 0; c < ch_to_copy; c++) {
                    double v = s0[c] + frac * (s1[c] - s0[c]);
                    out[f * out_channels + c] = (float)(v / 32768.0);
                }

                g_resampler.phase += g_resampler.ratio;
            }

            /* Remove consumed input samples */
            resampler_compact();
        }
    }

    /* Drop excess blocks to keep latency low */
    uint32_t buffered = ring_available();
    while (buffered > TARGET_RING_FILL) {
        ring_read();
        buffered--;
    }

    return noErr;
}

static bool start_coreaudio(const char *device_name)
{
    g_device = find_device_by_name(device_name);
    if (g_device == kAudioObjectUnknown) {
        fprintf(stderr, "Device '%s' not found. Use --list-devices to see available devices.\n",
                device_name);
        return false;
    }

    AudioObjectPropertyAddress srProp = {
        kAudioDevicePropertyNominalSampleRate,
        kAudioDevicePropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };

    /* Try to set 44100 - avoids resampling if device allows it */
    Float64 sr = SAMPLE_RATE;
    AudioObjectSetPropertyData(g_device, &srProp, 0, NULL, sizeof(sr), &sr);

    /* Read back actual sample rate (another app may own it) */
    Float64 actual_sr = 0;
    UInt32 srSize = sizeof(actual_sr);
    OSStatus err = AudioObjectGetPropertyData(g_device, &srProp, 0, NULL, &srSize, &actual_sr);
    if (err != noErr) {
        fprintf(stderr, "Could not read device sample rate (err=%d)\n", (int)err);
        return false;
    }

    resampler_init(SAMPLE_RATE, actual_sr);

    if (g_resampler.active) {
        fprintf(stderr, "Device sample rate: %.0f Hz (resampling from %d Hz)\n",
                actual_sr, SAMPLE_RATE);
    } else {
        fprintf(stderr, "Device sample rate: %.0f Hz (no resampling needed)\n", actual_sr);
    }

    /* Set buffer size */
    UInt32 bufferFrames = FRAMES_PER_BLOCK;
    AudioObjectPropertyAddress bufProp = {
        kAudioDevicePropertyBufferFrameSize,
        kAudioDevicePropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };
    err = AudioObjectSetPropertyData(g_device, &bufProp, 0, NULL, sizeof(bufferFrames), &bufferFrames);
    if (err != noErr) {
        fprintf(stderr, "Warning: could not set buffer size to %d (err=%d)\n", FRAMES_PER_BLOCK, (int)err);
    }

    err = AudioDeviceCreateIOProcID(g_device, audio_io_proc, NULL, &g_io_proc_id);
    if (err != noErr) {
        fprintf(stderr, "AudioDeviceCreateIOProcID failed: %d\n", (int)err);
        return false;
    }

    err = AudioDeviceStart(g_device, g_io_proc_id);
    if (err != noErr) {
        fprintf(stderr, "AudioDeviceStart failed: %d\n", (int)err);
        return false;
    }

    fprintf(stderr, "CoreAudio output started on '%s'\n", device_name);
    return true;
}

static void stop_coreaudio(void)
{
    if (g_device != kAudioObjectUnknown && g_io_proc_id) {
        AudioDeviceStop(g_device, g_io_proc_id);
        AudioDeviceDestroyIOProcID(g_device, g_io_proc_id);
        g_io_proc_id = NULL;
    }
}

/* ============================================================================
 * UDP Receiver
 * ============================================================================ */

static int open_udp_receiver(void)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(UDP_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(sock);
        return -1;
    }

    /* Set receive timeout for clean shutdown */
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    return sock;
}

static void *receiver_thread(void *arg)
{
    int sock = *(int *)arg;
    uint8_t packet[PACKET_SIZE + 64];  /* Extra space for safety */
    uint32_t expected_seq = 0;
    bool first_packet = true;

    while (g_running) {
        ssize_t n = recv(sock, packet, sizeof(packet), 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            if (errno == EINTR)
                continue;
            perror("recv");
            break;
        }

        if ((size_t)n < sizeof(audio_packet_header_t))
            continue;

        audio_packet_header_t *hdr = (audio_packet_header_t *)packet;
        if (hdr->magic != AUDIO_PACKET_MAGIC)
            continue;

        if (hdr->channels != NUM_CHANNELS || hdr->frames != FRAMES_PER_BLOCK)
            continue;

        size_t expected_size = sizeof(audio_packet_header_t) +
                               hdr->frames * hdr->channels * (hdr->bits_per_sample / 8);
        if ((size_t)n < expected_size)
            continue;

        int16_t *pcm = (int16_t *)(packet + sizeof(audio_packet_header_t));

        if (first_packet) {
            expected_seq = hdr->sequence;
            first_packet = false;
            g_connected = true;
            fprintf(stderr, "Receiving audio from Move (seq=%u)\n", hdr->sequence);
        }

        /* Track dropped packets */
        if (hdr->sequence != expected_seq) {
            uint32_t gap = hdr->sequence - expected_seq;
            g_packets_dropped += gap;
        }
        expected_seq = hdr->sequence + 1;

        /* Write to ring buffer */
        if (!ring_full()) {
            ring_write(pcm);
        } else {
            g_packets_dropped++;
        }

        g_packets_received++;
        g_last_sequence = hdr->sequence;
    }

    return NULL;
}

/* ============================================================================
 * WAV Recording Mode
 * ============================================================================ */

static void record_wav(int sock, const char *wav_path, bool split, int duration_secs)
{
    FILE *wav_file = NULL;
    FILE *slot_files[7] = {NULL};  /* 4 slots + ME mix + Move native + combined */
    const char *slot_names[] = {"slot1", "slot2", "slot3", "slot4",
                                "me_mix", "move_native", "combined"};
    uint32_t data_bytes = 0;
    uint32_t slot_data_bytes[7] = {0};

    if (split) {
        /* Create per-slot WAV files */
        char path[512];
        const char *ext = strrchr(wav_path, '.');
        size_t base_len = ext ? (size_t)(ext - wav_path) : strlen(wav_path);
        const char *extension = ext ? ext : ".wav";

        for (int i = 0; i < 7; i++) {
            snprintf(path, sizeof(path), "%.*s_%s%s",
                     (int)base_len, wav_path, slot_names[i], extension);
            slot_files[i] = fopen(path, "wb");
            if (!slot_files[i]) {
                fprintf(stderr, "Failed to create %s: %s\n", path, strerror(errno));
                goto cleanup;
            }
            wav_write_header(slot_files[i], 2, SAMPLE_RATE, BITS_PER_SAMPLE, 0);
            fprintf(stderr, "Recording: %s\n", path);
        }
    } else {
        wav_file = fopen(wav_path, "wb");
        if (!wav_file) {
            fprintf(stderr, "Failed to create %s: %s\n", wav_path, strerror(errno));
            return;
        }
        wav_write_header(wav_file, NUM_CHANNELS, SAMPLE_RATE, BITS_PER_SAMPLE, 0);
        fprintf(stderr, "Recording: %s (%d channels)\n", wav_path, NUM_CHANNELS);
    }

    uint8_t packet[PACKET_SIZE + 64];
    time_t start = time(NULL);
    uint32_t blocks = 0;
    bool first_packet = true;
    uint32_t expected_seq = 0;

    fprintf(stderr, "Waiting for audio from Move...\n");

    while (g_running) {
        if (duration_secs > 0 && (time(NULL) - start) >= duration_secs)
            break;

        ssize_t n = recv(sock, packet, sizeof(packet), 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            if (errno == EINTR)
                continue;
            break;
        }

        if ((size_t)n < sizeof(audio_packet_header_t))
            continue;

        audio_packet_header_t *hdr = (audio_packet_header_t *)packet;
        if (hdr->magic != AUDIO_PACKET_MAGIC)
            continue;
        if (hdr->channels != NUM_CHANNELS || hdr->frames != FRAMES_PER_BLOCK)
            continue;

        size_t expected_size = sizeof(audio_packet_header_t) +
                               hdr->frames * hdr->channels * (hdr->bits_per_sample / 8);
        if ((size_t)n < expected_size)
            continue;

        int16_t *pcm = (int16_t *)(packet + sizeof(audio_packet_header_t));

        if (first_packet) {
            expected_seq = hdr->sequence;
            first_packet = false;
            fprintf(stderr, "Recording started (seq=%u)\n", hdr->sequence);
        }

        if (hdr->sequence != expected_seq) {
            uint32_t gap = hdr->sequence - expected_seq;
            fprintf(stderr, "Dropped %u packets\n", gap);
        }
        expected_seq = hdr->sequence + 1;

        if (split) {
            /* De-interleave and write per-slot stereo */
            int16_t stereo_buf[FRAMES_PER_BLOCK * 2];
            for (int slot = 0; slot < 7; slot++) {
                int ch_offset = slot * 2;
                for (int f = 0; f < FRAMES_PER_BLOCK; f++) {
                    stereo_buf[f * 2 + 0] = pcm[f * NUM_CHANNELS + ch_offset + 0];
                    stereo_buf[f * 2 + 1] = pcm[f * NUM_CHANNELS + ch_offset + 1];
                }
                fwrite(stereo_buf, sizeof(int16_t), FRAMES_PER_BLOCK * 2, slot_files[slot]);
                slot_data_bytes[slot] += FRAMES_PER_BLOCK * 2 * sizeof(int16_t);
            }
        } else {
            fwrite(pcm, sizeof(int16_t), FRAMES_PER_BLOCK * NUM_CHANNELS, wav_file);
            data_bytes += FRAMES_PER_BLOCK * NUM_CHANNELS * sizeof(int16_t);
        }

        blocks++;
        if ((blocks % 1000) == 0) {
            float secs = (float)(blocks * FRAMES_PER_BLOCK) / SAMPLE_RATE;
            fprintf(stderr, "\r  %.1f seconds recorded", secs);
        }
    }

    float total_secs = (float)(blocks * FRAMES_PER_BLOCK) / SAMPLE_RATE;
    fprintf(stderr, "\nRecording complete: %.1f seconds (%u blocks)\n", total_secs, blocks);

cleanup:
    if (split) {
        for (int i = 0; i < 7; i++) {
            if (slot_files[i]) {
                wav_update_sizes(slot_files[i], slot_data_bytes[i]);
                fclose(slot_files[i]);
            }
        }
    } else if (wav_file) {
        wav_update_sizes(wav_file, data_bytes);
        fclose(wav_file);
    }
}

/* ============================================================================
 * Status Display
 * ============================================================================ */

static void *status_thread(void *arg)
{
    (void)arg;
    uint32_t last_packets = 0;

    while (g_running) {
        sleep(2);
        if (!g_running)
            break;

        uint32_t cur = g_packets_received;
        uint32_t rate = (cur - last_packets) / 2;
        last_packets = cur;

        if (g_connected) {
            uint32_t buf_fill = ring_available();
            fprintf(stderr, "\r  [connected] %u pkts/s | buf: %u/%d | drops: %u | underruns: %u   ",
                    rate, buf_fill, RING_BLOCKS, g_packets_dropped, g_underruns);
        } else {
            fprintf(stderr, "\r  [waiting for audio from Move...]   ");
        }
    }
    fprintf(stderr, "\n");
    return NULL;
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(int argc, char *argv[])
{
    const char *device_name = DEFAULT_DEVICE_NAME;
    const char *wav_path = NULL;
    bool split = false;
    bool do_list_devices = false;
    int duration_secs = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--list-devices") == 0) {
            do_list_devices = true;
        } else if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
            device_name = argv[++i];
        } else if (strcmp(argv[i], "--wav") == 0 && i + 1 < argc) {
            wav_path = argv[++i];
        } else if (strcmp(argv[i], "--split") == 0) {
            split = true;
        } else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            duration_secs = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [options]\n"
                   "\n"
                   "Options:\n"
                   "  --list-devices          List available audio output devices\n"
                   "  --device <name>         Output device (default: %s)\n"
                   "  --wav <file>            Record to WAV file instead of audio device\n"
                   "  --split                 Record separate WAV per slot (with --wav)\n"
                   "  --duration <seconds>    Record for specified duration\n"
                   "  -h, --help              Show this help\n"
                   "\n"
                   "Channel layout:\n"
                   "   1-2:  Slot 1 L/R\n"
                   "   3-4:  Slot 2 L/R\n"
                   "   5-6:  Slot 3 L/R\n"
                   "   7-8:  Slot 4 L/R\n"
                   "   9-10: ME Stereo Mix L/R\n"
                   "  11-12: Move Native L/R (without ME)\n"
                   "  13-14: Combined L/R (post Master FX)\n",
                   argv[0], DEFAULT_DEVICE_NAME);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return 1;
        }
    }

    if (do_list_devices) {
        list_output_devices();
        return 0;
    }

    /* Install signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Open UDP receiver */
    int sock = open_udp_receiver();
    if (sock < 0)
        return 1;

    fprintf(stderr, "Listening on UDP port %d\n", UDP_PORT);

    if (wav_path) {
        /* WAV recording mode - single-threaded, no CoreAudio */
        record_wav(sock, wav_path, split, duration_secs);
    } else {
        /* Live streaming mode - CoreAudio output */
        if (!start_coreaudio(device_name)) {
            close(sock);
            return 1;
        }

        /* Start receiver thread */
        pthread_t recv_tid, status_tid;
        pthread_create(&recv_tid, NULL, receiver_thread, &sock);
        pthread_create(&status_tid, NULL, status_thread, NULL);

        fprintf(stderr, "Streaming to '%s'. Press Ctrl+C to stop.\n", device_name);

        /* Wait for threads */
        pthread_join(recv_tid, NULL);
        g_running = 0;
        pthread_join(status_tid, NULL);

        stop_coreaudio();
    }

    close(sock);

    fprintf(stderr, "Total: %u packets received, %u dropped, %u underruns\n",
            g_packets_received, g_packets_dropped, g_underruns);

    return 0;
}
