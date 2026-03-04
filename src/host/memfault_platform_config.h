#pragma once

/* Move Anything — Memfault SDK platform configuration
 *
 * This header is included by the SDK via MEMFAULT_PLATFORM_CONFIG_FILE.
 * Only metrics collection is enabled; coredump and trace features are disabled.
 */

/* No ELF build ID on this platform */
#define MEMFAULT_USE_GNU_BUILD_ID 0

/* Use default SDK log routing (calls memfault_platform_log which we implement) */
#define MEMFAULT_PLATFORM_HAS_LOG_CONFIG 0

/* Disable asserts in production (we run as an LD_PRELOAD shim) */
#define MEMFAULT_SDK_ASSERT_ENABLED 0

/* Disable coredump collection — metrics only */
#define MEMFAULT_COREDUMP_COLLECT_LOG_REGIONS 0
#define MEMFAULT_COREDUMP_COLLECT_HEAP_STATS 0
#define MEMFAULT_COREDUMP_COLLECT_TASK_WATCHDOG_REGION 0

/* Disable reboot tracking (not meaningful for an LD_PRELOAD shim) */
#define MEMFAULT_REBOOT_TRACKING_ENABLED 0

/* Disable task watchdog */
#define MEMFAULT_TASK_WATCHDOG_ENABLE 0

/* Event storage: 1KB is plenty for periodic heartbeats */
#define MEMFAULT_EVENT_STORAGE_RAM_SIZE 1024

/* Software type reported to Memfault dashboard */
#define MEMFAULT_SOFTWARE_TYPE "move-anything"

/* Project key — used by our upload code, not the SDK's HTTP client */
#define MEMFAULT_PROJECT_KEY "nRotbSqqeksOiiWmEmxYz4KZsxZ3hanu"
