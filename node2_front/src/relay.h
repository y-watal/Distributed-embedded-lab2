/*
 * RELAY (N2 only, RELAY_ENABLED in node_config.h): forwards a first copy to
 * the other bus when the committed graph requires a hop. Direct heartbeat
 * observations cover the interval before a new link change is committed.
 * Own thread at PRIO_RELAY.
 */
#ifndef RELAY_H
#define RELAY_H

#include <zephyr/sys/atomic.h>

struct relay_stats {
	atomic_t forwarded;
	atomic_t skipped; /* no hop needed, or destination bus degraded or down */
	atomic_t dropped; /* relay queue full */
	atomic_t raft_late; /* Raft fragment relayed without the reserve (wait ran out) */
};

extern struct relay_stats relay_stats;

int relay_init(void);

#endif /* RELAY_H */
