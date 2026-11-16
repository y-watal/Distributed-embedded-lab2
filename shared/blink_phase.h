/*
 * Lab 5: blinker and hazard phase from global time, the same on every node.
 *
 * A lamp is lit during the first half of each period of global_gettime(),
 * with periods aligned to global time zero. Every rate below divides one
 * second, so lamp edges also fall on the PPS edges. Front (N2) and rear (N3)
 * therefore switch at the same global instant; their misalignment is only the
 * clock-sync error plus thread wake-up latency, not CAN delivery of BLINK.
 *
 * BLINK frames from the cockpit still choose the mode (off, left, right,
 * hazard). Their phase bit is set from the same function for anyone reading
 * the bus, but the STM32s no longer use it.
 */
#ifndef SHARED_BLINK_PHASE_H
#define SHARED_BLINK_PHASE_H

#include <stdbool.h>
#include <stdint.h>

#define BLINK_TURN_HALF_MS    500 /* turn signals: 1 Hz, as in Lab 3 (R2.4) */
#define BLINK_HAZARD_HALF_MS  250 /* hazards: 2 Hz (checkoff 7, "hazards return to 2 Hz") */
#define BLINK_FAILURE_HALF_MS 125 /* failure mode, cockpit down: 4 Hz (checkoff 6) */

_Static_assert(1000 % (2 * BLINK_TURN_HALF_MS) == 0 &&
	       1000 % (2 * BLINK_HAZARD_HALF_MS) == 0 &&
	       1000 % (2 * BLINK_FAILURE_HALF_MS) == 0,
	       "blink periods must divide one second so lamp edges meet PPS edges");

/* Lamp on at this global time for a blink with this half period (0 = off). */
static inline bool blink_phase_on(uint64_t global_us, uint32_t half_ms)
{
	return half_ms != 0 && (global_us / (half_ms * 1000ull)) % 2u == 0;
}

/* Microseconds from global_us to the next lamp edge (> 0). */
static inline uint64_t blink_us_to_edge(uint64_t global_us, uint32_t half_ms)
{
	uint64_t half_us = half_ms * 1000ull;

	return half_us - global_us % half_us;
}

#endif /* SHARED_BLINK_PHASE_H */
