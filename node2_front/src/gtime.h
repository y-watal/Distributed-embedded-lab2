/*
 * Lab 5 Part 6 on the STM32: global time from the leader's REF.
 *
 * The model itself is shared/timesync.c (same code on the Pi). This module
 * supplies the local clock (k_cycle_get_32() extended to 64 bits, in us),
 * stamps outgoing REF frames, restamps REF frames N2 relays, and runs the PPS
 * task: at every whole second of global_gettime() it drives the PPS test point
 * high for PPS_PULSE_MS and queues a snapshot that the report prints as one
 * "SYNC:" line (global and local time at the edge, drift ratio, corrections).
 *
 * Same file on N2 and N3.
 */
#ifndef GTIME_H
#define GTIME_H

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/drivers/can.h>

int gtime_init(void);

/* Local free-running clock, microseconds since boot. */
uint64_t gtime_local_us(void);

/* Current global time in microseconds (leader's epoch). */
uint64_t global_gettime(void);

/* Dispatch thread: a REF frame, rx_cb timestamp in k_cycle_get_32() cycles. */
void gtime_on_ref(const struct can_frame *frame, uint32_t rx_cycles);

/* Just before a REF frame is handed to the MCP2515: write global time into it. */
void gtime_stamp_ref(uint8_t data[8]);

/* N2 relay: restamp a leader's REF with our own global time before forwarding,
 * so the far node does not see the relay delay as clock offset. */
void gtime_restamp_relayed_ref(struct can_frame *frame);

/* Leader, or a follower within holdover of its last REF. */
bool gtime_is_synced(void);

/* Report thread: prints any PPS snapshots captured since the last call. */
void gtime_report(void);

#endif /* GTIME_H */
