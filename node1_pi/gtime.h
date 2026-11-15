/*
 * Lab 5 Part 6 on the Pi: global time from the leader's REF, same model as
 * the STM32s (shared/timesync.c). Local clock is CLOCK_MONOTONIC in us.
 *
 * As leader (default boot) the Pi's global time is its local clock, and it
 * stamps each REF just before write(). As a follower (after an STM32 took
 * over) it tracks that leader's REF like any other node. A PPS thread wakes
 * at every whole second of global_gettime() and records a snapshot that the
 * report prints as one "SYNC:" line. PPS_GPIO (BCM number, -1 = none) drives
 * a PPS test point once a pin is assigned.
 */
#ifndef PI_GTIME_H
#define PI_GTIME_H

#include <stdbool.h>
#include <stdint.h>

#include "protocol.h"

int gtime_init(void);
uint64_t gtime_local_us(void);
uint64_t global_gettime(void);

/* can_rx thread: a REF frame read at local time rx_ns (CLOCK_MONOTONIC). */
void gtime_on_ref(uint32_t id, const uint8_t data[FRAME_DLC], uint64_t rx_ns);

/* Just before write(): put global time into a REF frame. */
void gtime_stamp_ref(uint8_t data[FRAME_DLC]);

bool gtime_is_synced(void);

/* Report thread: print the SYNC lines captured since the last call. */
void gtime_report(void);

#endif /* PI_GTIME_H */
