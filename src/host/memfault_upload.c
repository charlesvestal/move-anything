/*
 * Move Anything — Memfault chunk upload thread
 *
 * Background pthread that periodically drains serialized chunks from the
 * Memfault SDK packetizer and POSTs them to the Memfault chunks API
 * using fork/exec of the bundled curl binary.
 *
 * Chunks that fail to upload are spooled to disk for retry.
 * Follows the shadow_process.c pattern for thread lifecycle.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#include <time.h>

#include "memfault/components.h"
#include "memfault_metrics.h"
#include "unified_log.h"

#define UPLOAD_INTERVAL_S   60
#define CURL_TIMEOUT_S      10
#define MAX_CHUNK_SIZE      1024
#define SPOOL_DIR           "/data/UserData/move-anything/memfault/spool"
#define MAX_SPOOL_FILES     100
#define CURL_PATH           "/data/UserData/move-anything/bin/curl"
#define CHUNKS_API_FMT      "https://chunks.memfault.com/api/v0/chunks/%s"

static pthread_t s_upload_thread;
static volatile bool s_upload_running = false;
static char s_device_serial[64] = {0};

/* Write a chunk to the spool directory for later retry */
static void spool_chunk(const uint8_t *data, size_t len)
{
    /* Ensure spool dir exists */
    mkdir(SPOOL_DIR, 0755);

    /* Cap spool files */
    DIR *d = opendir(SPOOL_DIR);
    if (d) {
        int count = 0;
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] != '.') count++;
        }
        closedir(d);
        if (count >= MAX_SPOOL_FILES) {
            LOG_WARN("memfault", "Spool full (%d files), dropping chunk", count);
            return;
        }
    }

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    char path[256];
    snprintf(path, sizeof(path), SPOOL_DIR "/chunk_%ld_%ld.bin",
             (long)ts.tv_sec, ts.tv_nsec / 1000000);

    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(data, 1, len, f);
        fclose(f);
    }
}

/* Upload a single chunk via fork/exec curl. Returns true on success. */
static bool upload_chunk(const uint8_t *data, size_t len)
{
    char url[256];
    snprintf(url, sizeof(url), CHUNKS_API_FMT, s_device_serial);

    /* Write chunk to a temp file (curl reads --data-binary from it) */
    const char *tmp_path = "/data/UserData/move-anything/memfault/chunk_tmp.bin";
    FILE *f = fopen(tmp_path, "wb");
    if (!f) return false;
    fwrite(data, 1, len, f);
    fclose(f);

    pid_t pid = fork();
    if (pid < 0) return false;

    if (pid == 0) {
        /* Child — exec curl */
        char timeout_str[16];
        snprintf(timeout_str, sizeof(timeout_str), "%d", CURL_TIMEOUT_S);
        char header[128];
        snprintf(header, sizeof(header), "Memfault-Project-Key: %s", MEMFAULT_PROJECT_KEY);
        char data_arg[256];
        snprintf(data_arg, sizeof(data_arg), "@%s", tmp_path);

        execl(CURL_PATH, "curl",
              "-s", "-S",
              "--max-time", timeout_str,
              "-X", "POST",
              "-H", header,
              "-H", "Content-Type: application/octet-stream",
              "--data-binary", data_arg,
              url,
              (char *)NULL);
        _exit(127);  /* exec failed */
    }

    /* Parent — wait for curl */
    int status = 0;
    waitpid(pid, &status, 0);
    unlink(tmp_path);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        return true;
    }
    return false;
}

/* Try to upload any spooled chunks */
static void drain_spool(void)
{
    DIR *d = opendir(SPOOL_DIR);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char path[256];
        snprintf(path, sizeof(path), SPOOL_DIR "/%s", ent->d_name);

        FILE *f = fopen(path, "rb");
        if (!f) continue;

        uint8_t buf[MAX_CHUNK_SIZE];
        size_t len = fread(buf, 1, sizeof(buf), f);
        fclose(f);

        if (len > 0 && upload_chunk(buf, len)) {
            unlink(path);
        }
    }
    closedir(d);
}

static void *upload_thread_fn(void *arg)
{
    (void)arg;
    LOG_INFO("memfault", "Upload thread started (interval=%ds)", UPLOAD_INTERVAL_S);

    while (s_upload_running) {
        /* Sleep in 1s increments so we can exit promptly */
        for (int i = 0; i < UPLOAD_INTERVAL_S && s_upload_running; i++) {
            sleep(1);
        }
        if (!s_upload_running) break;

        /* Drain chunks from the SDK packetizer */
        uint8_t chunk_buf[MAX_CHUNK_SIZE];
        int chunks_sent = 0;
        while (s_upload_running) {
            size_t chunk_len = sizeof(chunk_buf);
            bool has_data = memfault_packetizer_get_chunk(chunk_buf, &chunk_len);
            if (!has_data) break;

            if (upload_chunk(chunk_buf, chunk_len)) {
                chunks_sent++;
            } else {
                /* Network failure — spool for retry */
                spool_chunk(chunk_buf, chunk_len);
                LOG_WARN("memfault", "Upload failed, spooled chunk (%zu bytes)", chunk_len);
                break;  /* Don't hammer a dead connection */
            }
        }

        if (chunks_sent > 0) {
            LOG_DEBUG("memfault", "Uploaded %d chunk(s)", chunks_sent);
        }

        /* Try spooled chunks */
        drain_spool();
    }

    LOG_INFO("memfault", "Upload thread stopped");
    return NULL;
}

void mf_upload_start(const char *device_serial)
{
    if (s_upload_running) return;

    strncpy(s_device_serial, device_serial, sizeof(s_device_serial) - 1);
    s_device_serial[sizeof(s_device_serial) - 1] = '\0';

    /* Ensure spool directory exists */
    mkdir("/data/UserData/move-anything/memfault", 0755);
    mkdir(SPOOL_DIR, 0755);

    s_upload_running = true;
    pthread_create(&s_upload_thread, NULL, upload_thread_fn, NULL);
}

void mf_upload_stop(void)
{
    if (!s_upload_running) return;
    s_upload_running = false;
    pthread_join(s_upload_thread, NULL);
}
