/*
 * link_subscriber.cpp — Standalone Link Audio subscriber
 *
 * Uses the Ableton Link SDK's LinkAudioSource to subscribe to Move's
 * per-track audio channels. This triggers Move to stream audio via
 * chnnlsv, which the shim's sendto() hook intercepts.
 *
 * To avoid the quantum launch delay (Move waits for the next downbeat
 * when numPeers > 0), the subscriber responds to signals from the shim:
 *   SIGUSR1 = transport stopped → disable Link (numPeers drops to 0)
 *   SIGUSR2 = transport started → re-enable Link (audio resumes)
 *
 * This way, Link is disabled while Move is stopped, so pressing Play
 * sees numPeers=0 and starts immediately. After play begins, Link is
 * re-enabled and audio FX resume within ~2 seconds.
 *
 * Running as a standalone process (not inside Move's LD_PRELOAD shim)
 * avoids the hook conflicts that caused SIGSEGV in the in-shim approach.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <ableton/LinkAudio.hpp>

#include <unistd.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static std::atomic<bool> g_running{true};

/* File-based IPC flags (signals are unreliable with Link SDK's threads) */
static const char *FLAG_DISABLE = "/data/UserData/move-anything/link-subscriber-disable";
static const char *FLAG_ENABLE  = "/data/UserData/move-anything/link-subscriber-enable";

/* Channel IDs discovered via callback — processed in main loop */
struct PendingChannel {
    ableton::ChannelId id;
    std::string peerName;
    std::string name;
};
static std::mutex g_pending_mu;
static std::vector<PendingChannel> g_pending_channels;
static std::atomic<bool> g_channels_changed{false};

static std::atomic<uint64_t> g_buffers_received{0};

static const char *PID_FILE = "/data/UserData/move-anything/link-subscriber-pid";

static void signal_handler(int sig)
{
    /* Use write() — async-signal-safe, unlike printf() */
    const char *msg = "link-subscriber: caught signal\n";
    switch (sig) {
        case SIGSEGV: msg = "link-subscriber: SIGSEGV\n"; break;
        case SIGBUS:  msg = "link-subscriber: SIGBUS\n"; break;
        case SIGABRT: msg = "link-subscriber: SIGABRT\n"; break;
        case SIGTERM: msg = "link-subscriber: SIGTERM\n"; break;
        case SIGINT:  msg = "link-subscriber: SIGINT\n"; break;
        case SIGUSR1: return;  /* unused — using file-based IPC instead */
        case SIGUSR2: return;
    }
    (void)write(STDOUT_FILENO, msg, strlen(msg));

    if (sig == SIGSEGV || sig == SIGBUS || sig == SIGABRT) {
        _exit(128 + sig);
    }
    g_running = false;
}

static bool is_link_audio_enabled()
{
    std::ifstream f("/data/UserData/move-anything/config/features.json");
    if (!f.is_open()) return false;
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    auto pos = content.find("\"link_audio_enabled\"");
    if (pos == std::string::npos) return false;
    auto colon = content.find(':', pos);
    if (colon == std::string::npos) return false;
    auto nl = content.find('\n', colon);
    return content.find("true", colon) < nl;
}

static void write_pid_file()
{
    FILE *f = fopen(PID_FILE, "w");
    if (!f) {
        printf("link-subscriber: failed to write PID file\n");
        return;
    }
    fprintf(f, "%d\n", getpid());
    fclose(f);
    printf("link-subscriber: wrote PID %d to %s\n", getpid(), PID_FILE);
}

static void remove_pid_file()
{
    unlink(PID_FILE);
}

int main()
{
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGINT, signal_handler);
    std::signal(SIGSEGV, signal_handler);
    std::signal(SIGBUS, signal_handler);
    std::signal(SIGABRT, signal_handler);
    std::signal(SIGUSR1, signal_handler);
    std::signal(SIGUSR2, signal_handler);

    setvbuf(stdout, NULL, _IOLBF, 0);

    if (!is_link_audio_enabled()) {
        return 0;
    }

    printf("link-subscriber: starting (signal-controlled mode)\n");

    write_pid_file();

    /* Wait for Move to be running before joining Link — if we create the
     * session first, our initial tempo (120 BPM) overwrites Move's project
     * tempo.  By waiting, Move creates the session and we adopt its tempo. */
    printf("link-subscriber: waiting for Move to start...\n");
    for (int wait = 0; wait < 60 && g_running; wait++) {
        /* Check if MoveOriginal process exists */
        FILE *p = popen("pgrep -f MoveOriginal", "r");
        if (p) {
            char buf[32];
            bool found = (fgets(buf, sizeof(buf), p) != nullptr);
            pclose(p);
            if (found) {
                printf("link-subscriber: Move detected, waiting 5s for Link init...\n");
                std::this_thread::sleep_for(std::chrono::seconds(5));
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    /* Join Link session and enable audio.
     * The BPM here only matters if no session exists yet — since Move starts
     * first, the subscriber adopts Move's tempo automatically. */
    ableton::LinkAudio link(120.0, "ME-Sub");
    link.enable(true);
    link.enableLinkAudio(true);

    /* Create a dummy sink so that our PeerAnnouncements include at least one
     * channel.  Move's Sink handler looks up ChannelRequest.peerId in
     * mPeerSendHandlers, which is only populated when a PeerAnnouncement
     * with channels is received.  Without this, forPeer() returns nullopt
     * and audio is silently never sent. */
    ableton::LinkAudioSink dummySink(link, "ME-Sub-Ack", 256);
    printf("link-subscriber: dummy sink created (triggers peer announcement)\n");

    /* Callback just records channel IDs — source creation deferred to main loop
     * because LinkAudioSource constructor isn't safe from the callback thread */
    link.setChannelsChangedCallback([&]() {
        auto channels = link.channels();
        std::lock_guard<std::mutex> lock(g_pending_mu);
        g_pending_channels.clear();
        for (const auto& ch : channels) {
            if (ch.peerName.find("Move") != std::string::npos) {
                g_pending_channels.push_back({ch.id, ch.peerName, ch.name});
            }
        }
        g_channels_changed = true;
        printf("link-subscriber: discovered %zu Move channels\n",
               g_pending_channels.size());
    });

    printf("link-subscriber: waiting for channel discovery...\n");

    /* Active sources — managed in main loop only */
    std::vector<ableton::LinkAudioSource> sources;

    uint64_t last_count = 0;
    int tick = 0;
    bool link_enabled = true;

    while (g_running) {
        /* Sleep in short bursts so we respond quickly to signals.
         * std::this_thread::sleep_for retries after EINTR on Linux,
         * so a single 500ms sleep won't wake up for signal flags. */
        for (int s = 0; s < 10 && g_running; s++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            /* Check file-based IPC flags between each 50ms nap */
            if (access(FLAG_DISABLE, F_OK) == 0 ||
                access(FLAG_ENABLE, F_OK) == 0)
                break;
        }
        tick++;

        /* Handle disable request (transport stopped → hide from Link peers).
         * ByeBye message causes immediate numPeers drop on Move's side.
         * Stay disabled until shim creates the enable flag file.
         *
         * Only disable when we're the only peer (numPeers <= 1 from Move's
         * perspective = just us). If Live or other real peers are connected,
         * keep Link enabled so quantum sync works correctly with them. */
        if (access(FLAG_DISABLE, F_OK) == 0) {
            unlink(FLAG_DISABLE);
            if (link_enabled) {
                std::size_t peers = link.numPeers();
                if (peers <= 1) {
                    printf("link-subscriber: disabling Link (transport stopped, "
                           "numPeers=%zu — no real peers)\n", peers);
                    sources.clear();
                    link.enable(false);
                    link_enabled = false;
                } else {
                    printf("link-subscriber: ignoring disable (numPeers=%zu — "
                           "real peers connected, keeping quantum sync)\n", peers);
                }
            }
        }

        /* Handle enable request from shim (created ~2s after MIDI Start,
         * well after Move has processed requestBeatAtTime with numPeers=0) */
        if (access(FLAG_ENABLE, F_OK) == 0) {
            unlink(FLAG_ENABLE);
            if (!link_enabled) {
                printf("link-subscriber: re-enabling Link (shim request)\n");
                link.enable(true);
                link_enabled = true;
            }
        }

        /* Skip source management while Link is disabled */
        if (!link_enabled) continue;

        /* Create sources when channels change */
        if (g_channels_changed.exchange(false)) {
            std::vector<PendingChannel> pending;
            {
                std::lock_guard<std::mutex> lock(g_pending_mu);
                pending = g_pending_channels;
            }

            /* Destroy old sources first */
            sources.clear();
            printf("link-subscriber: cleared old sources\n");

            /* Small delay to let SDK process the unsubscriptions */
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            /* Pre-allocate to avoid reallocation (which moves LinkAudioSource objects) */
            sources.reserve(pending.size());

            /* Create new sources one at a time */
            for (const auto& pc : pending) {
                printf("link-subscriber: subscribing to %s/%s...\n",
                       pc.peerName.c_str(), pc.name.c_str());

                try {
                    sources.emplace_back(link, pc.id,
                        [](ableton::LinkAudioSource::BufferHandle) {
                            g_buffers_received.fetch_add(1, std::memory_order_relaxed);
                        });
                    printf("link-subscriber: OK\n");
                } catch (const std::exception& e) {
                    printf("link-subscriber: ERROR: %s\n", e.what());
                } catch (...) {
                    printf("link-subscriber: ERROR (unknown)\n");
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            printf("link-subscriber: %zu sources active\n", sources.size());
        }

        if (tick % 60 == 0) {
            uint64_t count = g_buffers_received.load(std::memory_order_relaxed);
            if (count != last_count) {
                printf("link-subscriber: %llu audio buffers received (link=%s)\n",
                       (unsigned long long)count,
                       link_enabled ? "on" : "off");
                last_count = count;
            }
        }
    }

    sources.clear();
    remove_pid_file();
    printf("link-subscriber: shutting down (%llu total buffers)\n",
           (unsigned long long)g_buffers_received.load());
    return 0;
}
