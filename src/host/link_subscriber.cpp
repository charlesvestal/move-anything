/*
 * link_subscriber.cpp — Standalone Link Audio subscriber
 *
 * Uses the Ableton Link SDK's LinkAudioSource to subscribe to Move's
 * per-track audio channels. This triggers Move to stream audio via
 * chnnlsv, which the shim's sendto() hook intercepts.
 *
 * The subscriber stays always active — audio flows continuously.
 * The shim handles quantum avoidance by rewriting the subscriber's
 * ALIVE packets to ByeBye in its recvfrom hook, so Move never counts
 * the subscriber as a Link peer (numPeers stays 0).
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

static const char *TEMPO_FILE = "/data/UserData/move-anything/link-audio-tempo";

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
    const char *msg = "link-subscriber: caught signal\n";
    switch (sig) {
        case SIGSEGV: msg = "link-subscriber: SIGSEGV\n"; break;
        case SIGBUS:  msg = "link-subscriber: SIGBUS\n"; break;
        case SIGABRT: msg = "link-subscriber: SIGABRT\n"; break;
        case SIGTERM: msg = "link-subscriber: SIGTERM\n"; break;
        case SIGINT:  msg = "link-subscriber: SIGINT\n"; break;
        case SIGUSR1: return;
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

/* Read tempo from file written by shim. Returns 0 on failure. */
static double read_tempo_file()
{
    FILE *f = fopen(TEMPO_FILE, "r");
    if (!f) return 0;
    double tempo = 0;
    fscanf(f, "%lf", &tempo);
    fclose(f);
    if (tempo >= 20.0 && tempo <= 999.0) return tempo;
    return 0;
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

    printf("link-subscriber: starting (always-active mode)\n");

    write_pid_file();

    /* Wait for Move to be running before joining Link — if we create the
     * session first, our initial tempo overwrites Move's project tempo.
     * By waiting, Move creates the session and we adopt its tempo. */
    printf("link-subscriber: waiting for Move to start...\n");
    for (int wait = 0; wait < 60 && g_running; wait++) {
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

    /* Read project tempo from shim's tempo file, fall back to 120 BPM */
    double initial_tempo = read_tempo_file();
    if (initial_tempo <= 0) initial_tempo = 120.0;
    printf("link-subscriber: initial tempo = %.1f BPM\n", initial_tempo);

    /* Create Link instance — always enabled, shim handles quantum avoidance
     * by rewriting our ALIVE packets to ByeBye in its recvfrom hook */
    ableton::LinkAudio link(initial_tempo, "ME-Sub");
    link.enable(true);
    link.enableLinkAudio(true);
    printf("link-subscriber: Link enabled (numPeers=%zu)\n", link.numPeers());

    /* Create a dummy sink so that our PeerAnnouncements include at least one
     * channel.  Move's Sink handler looks up ChannelRequest.peerId in
     * mPeerSendHandlers, which is only populated when a PeerAnnouncement
     * with channels is received.  Without this, forPeer() returns nullopt
     * and audio is silently never sent. */
    ableton::LinkAudioSink dummySink(link, "ME-Sub-Ack", 256);
    printf("link-subscriber: dummy sink created (triggers peer announcement)\n");

    /* Callback records channel IDs — source creation deferred to main loop
     * because LinkAudioSource constructor isn't safe from the callback thread. */
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

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        tick++;

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

            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            sources.reserve(pending.size());

            for (const auto& pc : pending) {
                printf("link-subscriber: subscribing to %s/%s...\n",
                       pc.peerName.c_str(), pc.name.c_str());

                try {
                    sources.emplace_back(link, pc.id,
                        [](ableton::LinkAudioSource::BufferHandle) {
                            /* Deliberately empty — we only need the subscription
                             * to trigger Move's audio flow.  Counting or touching
                             * the buffer wastes CPU on this constrained device. */
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
                printf("link-subscriber: %llu buffers (peers=%zu)\n",
                       (unsigned long long)count,
                       link.numPeers());
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
