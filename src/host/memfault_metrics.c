/*
 * Move Anything — Memfault metrics collection
 *
 * Initializes the Memfault SDK (event storage + metrics) and provides
 * the heartbeat trigger called from the shim's ioctl loop.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>

#include "memfault/components.h"
#include "memfault_port.h"
#include "memfault_metrics.h"
#include "unified_log.h"

/* RAM backing for Memfault event storage */
static uint8_t s_event_storage[MEMFAULT_EVENT_STORAGE_RAM_SIZE];

static bool s_initialized = false;

void mf_metrics_init(void)
{
    if (s_initialized) return;

    /* Read device serial and firmware version */
    mf_port_init();

    /* Boot event storage */
    const sMemfaultEventStorageImpl *storage =
        memfault_events_storage_boot(s_event_storage, sizeof(s_event_storage));

    /* Boot metrics (heartbeat timer handled by our manual trigger) */
    memfault_metrics_boot(storage, NULL);

    s_initialized = true;
    LOG_INFO("memfault", "Metrics initialized (storage=%zu bytes)", sizeof(s_event_storage));
}

void mf_metrics_shutdown(void)
{
    s_initialized = false;
}

void mf_metrics_collect_heartbeat(void)
{
    if (!s_initialized) return;

    /* Read crash count from persistent file */
    FILE *f = fopen("/data/UserData/move-anything/memfault/crash_count", "r");
    if (f) {
        unsigned int count = 0;
        if (fscanf(f, "%u", &count) == 1) {
            MEMFAULT_METRIC_SET_UNSIGNED(crash_count, count);
        }
        fclose(f);
    }

    /* Trigger the SDK to serialize current values and reset counters */
    memfault_metrics_heartbeat_debug_trigger();

    LOG_DEBUG("memfault", "Heartbeat collected");
}

bool mf_metrics_enabled(void)
{
    return s_initialized;
}
