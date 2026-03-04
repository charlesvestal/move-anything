/*
 * Move Anything — Memfault platform port
 *
 * Implements the platform abstraction functions required by the Memfault
 * Firmware SDK: device info, timekeeping, and log routing.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <stdbool.h>

#include "memfault/components.h"
#include "memfault_port.h"
#include "unified_log.h"

/* ---------- Cached device info ---------- */

static char s_device_serial[64] = "unknown";
static char s_fw_version[32] = "0.0.0";

void mf_port_init(void)
{
    /* Read machine-id for a stable per-device serial */
    FILE *f = fopen("/etc/machine-id", "r");
    if (f) {
        if (fgets(s_device_serial, sizeof(s_device_serial), f)) {
            /* Strip trailing newline */
            size_t len = strlen(s_device_serial);
            if (len > 0 && s_device_serial[len - 1] == '\n')
                s_device_serial[len - 1] = '\0';
        }
        fclose(f);
    }

    /* Read firmware version from the deployed version.txt */
    f = fopen("/data/UserData/move-anything/host/version.txt", "r");
    if (f) {
        if (fgets(s_fw_version, sizeof(s_fw_version), f)) {
            size_t len = strlen(s_fw_version);
            if (len > 0 && s_fw_version[len - 1] == '\n')
                s_fw_version[len - 1] = '\0';
        }
        fclose(f);
    }
}

/* ---------- SDK platform callbacks ---------- */

void memfault_platform_get_device_info(sMemfaultDeviceInfo *info)
{
    *info = (sMemfaultDeviceInfo){
        .device_serial = s_device_serial,
        .software_type = MEMFAULT_SOFTWARE_TYPE,
        .software_version = s_fw_version,
        .hardware_version = "move-1",
    };
}

uint64_t memfault_platform_get_time_since_boot_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* SDK log routing — forward to the unified logger */
void memfault_platform_log(eMemfaultPlatformLogLevel level, const char *fmt, ...)
{
    int ul_level;
    switch (level) {
        case kMemfaultPlatformLogLevel_Error:   ul_level = LOG_LEVEL_ERROR; break;
        case kMemfaultPlatformLogLevel_Warning: ul_level = LOG_LEVEL_WARN;  break;
        case kMemfaultPlatformLogLevel_Info:    ul_level = LOG_LEVEL_INFO;  break;
        default:                                ul_level = LOG_LEVEL_DEBUG; break;
    }

    va_list args;
    va_start(args, fmt);
    unified_log_v("memfault", ul_level, fmt, args);
    va_end(args);
}

/* Metrics timer — we trigger heartbeats manually from the ioctl loop,
 * so nothing to do here. */
bool memfault_platform_metrics_timer_boot(
    uint32_t period_sec,
    MEMFAULT_UNUSED MemfaultPlatformTimerCallback callback)
{
    (void)period_sec;
    (void)callback;
    return true;  /* Tell SDK the timer is "running" */
}

/* Non-volatile event storage — not used (we rely on RAM event storage only).
 * Provide a no-op so the SDK links cleanly. */
void memfault_platform_event_storage_read(
    uint32_t offset, void *buf, size_t buf_len)
{
    (void)offset;
    memset(buf, 0, buf_len);
}
