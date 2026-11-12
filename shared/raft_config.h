/* Lab 5 Raft defaults. Include this on all three nodes, independently of CAN. */
#ifndef SHARED_RAFT_CONFIG_H
#define SHARED_RAFT_CONFIG_H

#include "protocol.h"

/* The common nodes list is NODE_N1, NODE_N2, NODE_N3 in protocol.h. */
#define RAFT_CLUSTER_SIZE              NUM_NODES
#ifndef RAFT_DEFAULT_LEADER
#define RAFT_DEFAULT_LEADER            NODE_N1 /* cockpit; -1 selects election at boot */
#endif
#define RAFT_HEARTBEAT_MS              20
#define RAFT_EMPTY_APPEND_MS           100
#define RAFT_MISSED_HEARTBEATS         3
#define RAFT_ELECTION_JITTER_MS        39 /* random inclusive range: 0..39 ms */
#define RAFT_LOG_CAPACITY              30000
#define RAFT_DEBUG                     1

_Static_assert(RAFT_CLUSTER_SIZE == 3, "Lab 5 requires three nodes");
_Static_assert(RAFT_DEFAULT_LEADER >= -1 && RAFT_DEFAULT_LEADER < RAFT_CLUSTER_SIZE,
	       "default leader must be a cluster node or -1");

#endif /* SHARED_RAFT_CONFIG_H */
