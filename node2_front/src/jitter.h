/*
 * Queuing jitter J per message: time from the event that triggers a frame to
 * the moment it is queued in the MCP2515 (can_send() returned 0, i.e. the TX
 * buffer is loaded and RTS issued), per bus. Wall-clock (k_cycle_get_32(),
 * 84 MHz), so preemption and blocking ARE included - that is what J is.
 *
 * Release (trigger) points:
 *   HB_Nx     the 20 ms k_timer expiry (hb_timer_fn)
 *   FORCE     the 50 ms k_timer expiry (steer.c)
 *   FAULT_Nx  the fault being recognised in zone_update(); for the test
 *             button, the GPIO interrupt. Includes any wait for the 20 ms
 *             FAULT spacing (see "last 5 s" to isolate one press)
 *   relayed   (N2) the frame reaching rx_cb, after the driver's SPI read. Add
 *             the driver job (INT -> rx_cb) for the full relay delay
 *
 * The REPORT thread prints max since boot, max over the last JITTER_REPORT_EVERY
 * seconds, mean and count. JITTER_MEASURE 0 (lab3_config.h) compiles it away;
 * canbus_send_at() / canbus_forward_at() still exist and just don't record.
 */
#ifndef JITTER_H
#define JITTER_H

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/kernel.h>

#include "lab3_config.h"

/* A release timestamp for canbus_send_at(); never 0 (0 means "don't record"). */
static inline uint32_t jit_now(void)
{
	uint32_t t = k_cycle_get_32();

	return t != 0 ? t : 1;
}

#if JITTER_MEASURE

/* Called by canbus.c right after a frame was queued on bus. */
void jit_note(uint32_t id, int bus, uint32_t release, bool relayed);

/* Prints the table (REPORT thread) and starts a new "last" window. */
void jit_report(void);

#else

static inline void jit_note(uint32_t id, int bus, uint32_t release, bool relayed)
{
	ARG_UNUSED(id);
	ARG_UNUSED(bus);
	ARG_UNUSED(release);
	ARG_UNUSED(relayed);
}
static inline void jit_report(void) {}

#endif /* JITTER_MEASURE */

#endif /* JITTER_H */
