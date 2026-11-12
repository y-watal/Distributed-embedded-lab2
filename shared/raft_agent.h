#ifndef SHARED_RAFT_AGENT_H
#define SHARED_RAFT_AGENT_H

#include "raft_config.h"
#include "raft_wire.h"

/* Step 2 working log. Durable 30,000-entry storage is a later lab step. */
#define RAFT_WORKING_LOG_ENTRIES 64
#define RAFT_CLIENT_QUEUE_LEN 16

enum raft_role {
	RAFT_FOLLOWER,
	RAFT_CANDIDATE,
	RAFT_LEADER,
};

struct raft_log_entry {
	uint32_t term;
	struct raft_event event;
};

struct raft_network {
	bool node_up[NUM_NODES];
	bool node_known[NUM_NODES];
	bool link_up[NUM_NODES][2];
	bool link_known[NUM_NODES][2];
	bool cockpit_ok;
	bool cockpit_known;
};

/* One debounced subject: cockpit, a node, or one node's link to one bus. */
#define RAFT_OBS_SUBJECTS (1 + NUM_NODES + NUM_NODES * 2)
struct raft_observation {
	bool seen;        /* at least one observation arrived */
	bool up;          /* latest raw observation */
	bool reported;    /* a debounced value has been queued */
	bool reported_up; /* that value */
	uint32_t since_ms;
};

/* Sends one complete logical body. The platform adapter fragments it for CAN. */
typedef bool (*raft_send_fn)(void *ctx, uint32_t can_id, const uint8_t *body, uint8_t len);

struct raft_agent {
	uint8_t self;
	int8_t leader;
	int8_t voted_for;
	enum raft_role role;
	uint32_t term;
	uint32_t last_index;
	uint32_t commit_index;
	uint32_t applied_index;
	uint32_t last_ref_ms;
	uint32_t last_ref_send_ms;
	uint32_t last_empty_append_ms;
	uint32_t election_deadline_ms;
	uint32_t election_random_state;
	uint16_t ref_cycle;
	bool ref_sent;
	bool empty_append_sent;
	bool election_timer_active;
	struct raft_log_entry log[RAFT_WORKING_LOG_ENTRIES];
	struct raft_network network;
	struct raft_event queued[RAFT_CLIENT_QUEUE_LEN];
	uint8_t queue_head, queue_count;
	uint32_t next_request_id;
	uint32_t active_request_id;
	struct raft_event active_event;
	bool active_request;
	uint32_t last_client_send_ms;
	uint32_t client_retry_ms;
	struct raft_observation obs[RAFT_OBS_SUBJECTS];
	uint32_t pending_index;
	uint8_t pending_client;
	uint32_t pending_request_id;
	uint8_t pending_acks; /* bit per node */
	uint32_t last_append_send_ms;
	uint32_t last_client_id[NUM_NODES];
	uint32_t last_client_index[NUM_NODES];
	uint8_t votes; /* bit per node */
	raft_send_fn send;
	void *send_ctx;
};

/* Starts with the configured leader, or schedules a randomized boot election. */
void raft_agent_init(struct raft_agent *a, uint8_t self, raft_send_fn send, void *send_ctx,
		     uint32_t now_ms, uint32_t random_seed);

/* Accept one fully reassembled Lab 5 message, with the sender in the CAN ID. */
void raft_agent_receive(struct raft_agent *a, uint32_t can_id, const uint8_t *body,
			uint8_t len, uint32_t now_ms);

/*
 * Queue an event for the leader now. A queued event about the same cockpit,
 * node, or link is replaced rather than queued behind, so only the latest
 * state is sent.
 */
bool raft_client_report(struct raft_agent *a, struct raft_event event);

/*
 * Record a local observation (an edge from the Lab 3 monitors). Cockpit,
 * node, and link observations are reported only after holding for
 * RAFT_REPORT_DOWN_HOLD_MS or RAFT_REPORT_UP_HOLD_MS; a flicker shorter than
 * that is never logged. Other events go straight to raft_client_report().
 */
bool raft_client_observe(struct raft_agent *a, struct raft_event event, uint32_t now_ms);

/* Drive client retries and pending replication from a low-priority worker. */
void raft_agent_poll(struct raft_agent *a, uint32_t now_ms);

/* Time until the next heartbeat or election action, at most 20 ms. */
uint32_t raft_agent_next_wake_ms(const struct raft_agent *a, uint32_t now_ms);

/* Client read of the locally committed network graph. */
void raft_client_get_network(const struct raft_agent *a, struct raft_network *out);

/* Election entry point, also used by the missed-heartbeat timer. */
void raft_agent_start_election(struct raft_agent *a);

#endif /* SHARED_RAFT_AGENT_H */
