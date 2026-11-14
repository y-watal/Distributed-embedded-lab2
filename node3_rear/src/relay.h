/*
 * RELAY (RELAY_ENABLED in node_config.h, on every STM32; the Pi does the same
 * in cockpit.c): forwards a first copy to the other bus when the committed
 * graph makes this node the bridge for its sender and some node can only hear
 * it that way (raft_route_relay). Own thread at PRIO_RELAY.
 */
#ifndef RELAY_H
#define RELAY_H

#include <zephyr/sys/atomic.h>

struct relay_stats {
	atomic_t forwarded;
	atomic_t skipped; /* no hop needed, or destination bus degraded or down */
	atomic_t dropped; /* relay queue full */
	atomic_t raft_late; /* Raft fragment relayed without the reserve (wait ran out) */
	atomic_t full;      /* gave up: no TX buffer within RELAY_*_WAIT_US */
};

extern struct relay_stats relay_stats;

int relay_init(void);

#endif /* RELAY_H */
