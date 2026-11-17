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
#define RAFT_LOG_CAPACITY              30000 /* log_size: lifetime entries before updates are refused */
/*
 * Entries kept in RAM. Older committed entries are compacted into a snapshot
 * (network state + log hash at that index); a follower that needs one gets
 * the snapshot instead (APPEND with RAFT_SNAPSHOT_MARK). 256 x 8 bytes fits
 * the STM32s; 30,000 entries would not fit in their 96 KB of RAM.
 */
#define RAFT_LOG_WINDOW                256
#ifndef RAFT_DEBUG
#define RAFT_DEBUG                     1 /* Debug: print election and append messages */
#endif
/*
 * The default leader listens this long before taking term 1. If it hears a
 * running cluster (REF or APPEND from another node), it joins as a follower
 * instead: that is the Pi coming back after an STM32 took over. Longer than
 * the 100 ms empty-APPEND period, so a running leader is always heard.
 */
#define RAFT_BOOT_LISTEN_MS            250
/*
 * Random-leader mode (RAFT_DEFAULT_LEADER -1): the boot election timer starts
 * once this node hears a majority (itself + one peer), so any two nodes elect
 * a leader without the third. Nodes that come up together start their clocks
 * together, and then the seed decides. RAFT_ELECTION_SEED 0 draws a
 * hardware-random timeout; any other value makes every node's timeouts
 * reproducible. RAFT_SEED_PI_WINS gives the cockpit the shortest one: start
 * the cockpit first, then reset both STM32s, so all three meet at once.
 */
#ifndef RAFT_ELECTION_SEED
#define RAFT_ELECTION_SEED             0
#endif
/* With this seed the first boot timeouts are N1 60 ms, N2 95 ms, N3 95 ms
 * (tests/raft_random_test.c checks it). Build with SEED=408 to use it. */
#define RAFT_SEED_PI_WINS              408u
/* Checkoff 8 "an STM32 is leader": first timeouts N1 99, N2 64, N3 96 ms. */
#define RAFT_SEED_N2_WINS              14u
/* N1 98, N2 97, N3 65 ms. */
#define RAFT_SEED_N3_WINS              112u

/*
 * Raft client pacing. Lab 3 DEG and peer-lost detection stay fast for local
 * behaviour; only the replicated log is debounced, so a single late heartbeat
 * does not become two permanent log entries.
 */
#define RAFT_REPORT_DOWN_HOLD_MS       50   /* loss must persist this long after detection */
#define RAFT_REPORT_UP_HOLD_MS         100  /* recovery must stay stable this long */
#define RAFT_CLIENT_RETRY_MS           100  /* resend an unanswered CLIENT_UPDATE */
#define RAFT_CLIENT_REJECT_BACKOFF_MS  1000 /* after the leader answers REJECTED */
#define RAFT_CLIENT_REPAIR_HOLD_MS     1000 /* after COMMITTED, before re-reporting a graph disagreement */

_Static_assert(RAFT_CLUSTER_SIZE == 3, "Lab 5 requires three nodes");
_Static_assert(RAFT_DEFAULT_LEADER >= -1 && RAFT_DEFAULT_LEADER < RAFT_CLUSTER_SIZE,
	       "default leader must be a cluster node or -1");

#endif /* SHARED_RAFT_CONFIG_H */
