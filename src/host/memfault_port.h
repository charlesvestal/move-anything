#ifndef MEMFAULT_PORT_H
#define MEMFAULT_PORT_H

/*
 * Move Anything — Memfault platform port header
 *
 * Declares the platform abstraction functions required by the Memfault SDK.
 * Implementation in memfault_port.c.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Read /etc/machine-id and cache it for device_serial.
 * Called once during mf_metrics_init(). */
void mf_port_init(void);

#ifdef __cplusplus
}
#endif

#endif /* MEMFAULT_PORT_H */
