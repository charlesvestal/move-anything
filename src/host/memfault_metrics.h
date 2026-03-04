#ifndef MEMFAULT_METRICS_H
#define MEMFAULT_METRICS_H

/*
 * Move Anything — Memfault metrics collection API
 *
 * Provides compile-time gated wrappers around the Memfault SDK.
 * When MEMFAULT_ENABLED is not defined, all calls become no-ops.
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef MEMFAULT_ENABLED

/* Initialize Memfault SDK and metrics subsystem.
 * Call once after unified_log_init(). */
void mf_metrics_init(void);

/* Shutdown (currently a no-op, reserved for future use). */
void mf_metrics_shutdown(void);

/* Trigger a heartbeat: serializes current metric values and queues
 * them for upload. Call from the existing heartbeat_counter path. */
void mf_metrics_collect_heartbeat(void);

/* Returns true if metrics are initialized and the runtime feature flag
 * is enabled. */
bool mf_metrics_enabled(void);

/* Start the background upload thread. */
void mf_upload_start(const char *device_serial);

/* Stop the background upload thread. */
void mf_upload_stop(void);

#else /* !MEMFAULT_ENABLED */

static inline void mf_metrics_init(void) {}
static inline void mf_metrics_shutdown(void) {}
static inline void mf_metrics_collect_heartbeat(void) {}
static inline bool mf_metrics_enabled(void) { return false; }
static inline void mf_upload_start(const char *s) { (void)s; }
static inline void mf_upload_stop(void) {}

#endif /* MEMFAULT_ENABLED */

#ifdef __cplusplus
}
#endif

#endif /* MEMFAULT_METRICS_H */
