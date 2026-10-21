/*
 * RELAY (N2 only, RELAY_ENABLED in node_config.h): forwards the first copy of
 * every frame from another node, bit-identical, to the other bus. Always on,
 * no fault detection: in R4.8 scenario (c) the forwarded copies are already
 * flowing when the second jumper comes out. Own thread at PRIO_RELAY.
 */
#ifndef RELAY_H
#define RELAY_H

#include <zephyr/sys/atomic.h>

struct relay_stats {
	atomic_t forwarded;
	atomic_t skipped; /* destination bus degraded or down */
	atomic_t dropped; /* relay queue full */
};

extern struct relay_stats relay_stats;

int relay_init(void);

#endif /* RELAY_H */
