#include "raft_agent.h"

#include <string.h>

static bool send_body(struct raft_agent *a, uint32_t id, const uint8_t *body, uint8_t len)
{
	return a->send && a->send(a->send_ctx, id, body, len);
}

/* RAFT_DEBUG: election and append messages (handout config key "Debug"). */
#define DBG(a, ...)                                                       \
	do {                                                              \
		if (RAFT_DEBUG && (a)->debug) {                           \
			(a)->debug((a)->debug_ctx, __VA_ARGS__);          \
		}                                                         \
	} while (0)

uint32_t raft_hash_entry(uint32_t hash, const struct raft_log_entry *e)
{
	const uint8_t event[] = {e->event.type, e->event.node, e->event.bus, e->event.value};

	for (int byte = 0; byte < 4; byte++) {
		hash = (hash ^ (uint8_t)(e->term >> (8 * byte))) * 16777619u;
	}
	for (int byte = 0; byte < 4; byte++) {
		hash = (hash ^ event[byte]) * 16777619u;
	}
	return hash;
}

uint32_t raft_network_pack(const struct raft_network *n)
{
	uint32_t bits = 0;

	for (int node = 0; node < NUM_NODES; node++) {
		bits |= (uint32_t)n->node_up[node] << node;
		bits |= (uint32_t)n->node_known[node] << (3 + node);
		for (int bus = 0; bus < 2; bus++) {
			bits |= (uint32_t)n->link_up[node][bus] << (6 + node * 2 + bus);
			bits |= (uint32_t)n->link_known[node][bus] << (12 + node * 2 + bus);
		}
	}
	bits |= (uint32_t)n->cockpit_ok << 18;
	bits |= (uint32_t)n->cockpit_known << 19;
	return bits;
}

void raft_network_unpack(uint32_t bits, struct raft_network *n)
{
	for (int node = 0; node < NUM_NODES; node++) {
		n->node_up[node] = (bits >> node) & 1u;
		n->node_known[node] = (bits >> (3 + node)) & 1u;
		for (int bus = 0; bus < 2; bus++) {
			n->link_up[node][bus] = (bits >> (6 + node * 2 + bus)) & 1u;
			n->link_known[node][bus] = (bits >> (12 + node * 2 + bus)) & 1u;
		}
	}
	n->cockpit_ok = (bits >> 18) & 1u;
	n->cockpit_known = (bits >> 19) & 1u;
}

/* Entry `index`, which must be in RAM (log_base < index <= last_index). */
static struct raft_log_entry *entry_at(struct raft_agent *a, uint32_t index)
{
	return &a->log[(index - 1u) % RAFT_LOG_WINDOW];
}

/* Term of entry `index`; 0 for index 0. Compacted entries before log_base are
 * committed, and only matched through prev_matches(). */
static uint32_t term_at(const struct raft_agent *a, uint32_t index)
{
	if (index == 0 || index < a->log_base) {
		return 0;
	}
	if (index == a->log_base) {
		return a->base_term;
	}
	return a->log[(index - 1u) % RAFT_LOG_WINDOW].term;
}

/* Does our log hold entry `index` with term `term`? Compacted entries are
 * committed, and committed entries match the leader's by the Raft rules. */
static bool prev_matches(const struct raft_agent *a, uint32_t index, uint32_t term)
{
	if (index == 0 || index < a->log_base) {
		return true;
	}
	return index <= a->last_index && term_at(a, index) == term;
}

bool raft_agent_entry(const struct raft_agent *a, uint32_t index, struct raft_log_entry *out)
{
	if (index <= a->log_base || index > a->last_index) {
		return false;
	}
	*out = a->log[(index - 1u) % RAFT_LOG_WINDOW];
	return true;
}

static void apply_event(struct raft_network *n, const struct raft_event *e)
{
	if (e->type == RAFT_EVENT_COCKPIT_OK) {
		n->cockpit_ok = true;
		n->cockpit_known = true;
	} else if (e->type == RAFT_EVENT_COCKPIT_ERROR) {
		n->cockpit_ok = false;
		n->cockpit_known = true;
	} else if (e->node < NUM_NODES) {
		if (e->type == RAFT_EVENT_NODE_UP) {
			n->node_up[e->node] = true;
			n->node_known[e->node] = true;
		} else if (e->type == RAFT_EVENT_NODE_DOWN) {
			n->node_up[e->node] = false;
			n->node_known[e->node] = true;
			n->link_up[e->node][0] = false;
			n->link_up[e->node][1] = false;
			n->link_known[e->node][0] = true;
			n->link_known[e->node][1] = true;
		} else if (e->bus < 2 && e->type == RAFT_EVENT_CANLINK_UP) {
			n->link_up[e->node][e->bus] = true;
			n->link_known[e->node][e->bus] = true;
		} else if (e->bus < 2 && e->type == RAFT_EVENT_CANLINK_DOWN) {
			n->link_up[e->node][e->bus] = false;
			n->link_known[e->node][e->bus] = true;
		}
	}
}

static bool event_already_applied(const struct raft_network *n, const struct raft_event *e)
{
	if (e->type == RAFT_EVENT_COCKPIT_OK || e->type == RAFT_EVENT_COCKPIT_ERROR) {
		return n->cockpit_known && n->cockpit_ok == (e->type == RAFT_EVENT_COCKPIT_OK);
	}
	if (e->node >= NUM_NODES) {
		return false;
	}
	if (e->type == RAFT_EVENT_NODE_UP || e->type == RAFT_EVENT_NODE_DOWN) {
		return n->node_known[e->node] &&
		       n->node_up[e->node] == (e->type == RAFT_EVENT_NODE_UP);
	}
	if (e->bus < 2 &&
	    (e->type == RAFT_EVENT_CANLINK_UP || e->type == RAFT_EVENT_CANLINK_DOWN)) {
		return n->link_known[e->node][e->bus] &&
		       n->link_up[e->node][e->bus] == (e->type == RAFT_EVENT_CANLINK_UP);
	}
	return false;
}

/* Debounce subject for cockpit, node, and link events; -1 for anything else. */
static int event_subject(const struct raft_event *e, bool *up)
{
	switch (e->type) {
	case RAFT_EVENT_COCKPIT_OK:
	case RAFT_EVENT_COCKPIT_ERROR:
		*up = e->type == RAFT_EVENT_COCKPIT_OK;
		return 0;
	case RAFT_EVENT_NODE_UP:
	case RAFT_EVENT_NODE_DOWN:
		*up = e->type == RAFT_EVENT_NODE_UP;
		return e->node < NUM_NODES ? 1 + e->node : -1;
	case RAFT_EVENT_CANLINK_UP:
	case RAFT_EVENT_CANLINK_DOWN:
		*up = e->type == RAFT_EVENT_CANLINK_UP;
		return e->node < NUM_NODES && e->bus < 2 ?
		       1 + NUM_NODES + e->node * 2 + e->bus : -1;
	default:
		return -1;
	}
}

static struct raft_event subject_event(int subject, bool up)
{
	if (subject == 0) {
		return (struct raft_event){up ? RAFT_EVENT_COCKPIT_OK : RAFT_EVENT_COCKPIT_ERROR,
					   NODE_N1, RAFT_BUS_NONE, 0};
	}
	if (subject <= NUM_NODES) {
		return (struct raft_event){up ? RAFT_EVENT_NODE_UP : RAFT_EVENT_NODE_DOWN,
					   (uint8_t)(subject - 1), RAFT_BUS_NONE, 0};
	}
	int link = subject - 1 - NUM_NODES;

	return (struct raft_event){up ? RAFT_EVENT_CANLINK_UP : RAFT_EVENT_CANLINK_DOWN,
				   (uint8_t)(link / 2), (uint8_t)(link % 2), 0};
}

static void apply_committed(struct raft_agent *a)
{
	while (a->applied_index < a->commit_index && a->applied_index < a->last_index) {
		uint32_t index = a->applied_index + 1;
		const struct raft_log_entry *e = entry_at(a, index);

		apply_event(&a->network, &e->event);
		a->applied_hash = raft_hash_entry(a->applied_hash, e);
		a->applied_index = index;
		if (a->on_apply) {
			a->on_apply(a->apply_ctx, index, e, index <= a->replay_boundary, &a->network);
		}
	}
}

/*
 * Make room in the RAM window for entry `index` by folding the oldest applied
 * entries into the base snapshot. Only applied (so committed) entries are
 * ever compacted.
 */
static void compact_for(struct raft_agent *a, uint32_t index)
{
	while (index - a->log_base > RAFT_LOG_WINDOW && a->log_base < a->applied_index) {
		uint32_t index = a->log_base + 1;
		const struct raft_log_entry *e = entry_at(a, index);

		apply_event(&a->base_network, &e->event);
		a->base_hash = raft_hash_entry(a->base_hash, e);
		a->base_term = e->term;
		a->log_base = index;
	}
}

/* Room to store entry `index` (compacting if needed)? */
static bool room_for(struct raft_agent *a, uint32_t index)
{
	compact_for(a, index);
	return index > a->log_base && index - a->log_base <= RAFT_LOG_WINDOW;
}

static void reply_client(struct raft_agent *a, uint8_t target, uint32_t request_id,
			 enum raft_client_status status, uint32_t index)
{
	struct raft_client_reply reply = {
		.target = target,
		.request_id = request_id,
		.status = status,
		.leader = a->leader < 0 ? RAFT_TARGET_ALL : (uint8_t)a->leader,
		.committed_index = index,
	};
	uint8_t body[RAFT_CLIENT_REPLY_LEN];

	raft_client_reply_encode(&reply, body);
	send_body(a, ID_CLIENT_REPLY(a->self), body, sizeof(body));
}

/* The leader's own client is answered in place instead of over CAN. */
static void client_result(struct raft_agent *a, uint8_t client, uint32_t request_id,
			  enum raft_client_status status, uint32_t index)
{
	if (client != a->self) {
		reply_client(a, client, request_id, status, index);
		return;
	}
	if (!a->active_request || request_id != a->active_request_id) {
		return;
	}
	if (status == RAFT_CLIENT_COMMITTED) {
		a->active_request = false;
	} else if (status == RAFT_CLIENT_REJECTED) {
		a->client_retry_ms = RAFT_CLIENT_REJECT_BACKOFF_MS;
	}
}

/* InstallSnapshot: the follower needs an entry we have compacted away. */
static void send_snapshot(struct raft_agent *a, uint8_t target)
{
	struct raft_append m = {
		.target = target,
		.term = a->term,
		.prev_index = a->log_base,
		.prev_term = a->base_term,
		.leader_commit = a->commit_index,
		.entry_count = RAFT_SNAPSHOT_MARK,
		.snapshot_network = raft_network_pack(&a->base_network),
		.snapshot_hash = a->base_hash,
	};
	uint8_t body[RAFT_APPEND_SNAPSHOT_LEN];
	uint8_t len = raft_append_encode(&m, body);

	DBG(a, "append: snapshot through #%u (term %u) -> N%u", (unsigned)a->log_base,
	    (unsigned)a->base_term, (unsigned)target + 1);
	send_body(a, ID_APPEND(a->self), body, len);
}

static void send_append(struct raft_agent *a, uint8_t target, uint32_t index)
{
	if (index == 0 || index > a->last_index) {
		return;
	}
	if (index <= a->log_base) {
		send_snapshot(a, target);
		return;
	}
	const struct raft_log_entry *e = entry_at(a, index);
	struct raft_append m = {
		.target = target,
		.term = a->term,
		.prev_index = index - 1,
		.prev_term = term_at(a, index - 1),
		.leader_commit = a->commit_index,
		.entry_count = 1,
		.entry_term = e->term,
		.entry = e->event,
	};
	uint8_t body[RAFT_APPEND_ENTRY_LEN];
	uint8_t len = raft_append_encode(&m, body);

	if (target == RAFT_TARGET_ALL) {
		DBG(a, "append: #%u term %u %s -> all", (unsigned)index, (unsigned)e->term,
		    raft_event_name(e->event.type));
	} else {
		DBG(a, "append: #%u term %u %s -> N%u", (unsigned)index, (unsigned)e->term,
		    raft_event_name(e->event.type), (unsigned)target + 1);
	}
	send_body(a, ID_APPEND(a->self), body, len);
}

static bool send_empty_append(struct raft_agent *a)
{
	struct raft_append m = {
		.target = RAFT_TARGET_ALL,
		.term = a->term,
		.prev_index = a->last_index,
		.prev_term = term_at(a, a->last_index),
		.leader_commit = a->commit_index,
	};
	uint8_t body[RAFT_APPEND_ENTRY_LEN];
	uint8_t len = raft_append_encode(&m, body);

	return send_body(a, ID_APPEND(a->self), body, len);
}

static uint32_t next_random(struct raft_agent *a)
{
	uint32_t x = a->election_random_state;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	a->election_random_state = x;
	return x;
}

static uint32_t election_delay_ms(struct raft_agent *a)
{
	return RAFT_MISSED_HEARTBEATS * RAFT_HEARTBEAT_MS +
	       next_random(a) % (RAFT_ELECTION_JITTER_MS + 1u);
}

static void reset_election_timer(struct raft_agent *a, uint32_t now_ms)
{
	a->election_deadline_ms = now_ms + election_delay_ms(a);
	a->election_timer_active = true;
}

static void send_ref(struct raft_agent *a, uint32_t now_ms)
{
	struct raft_ref ref = {
		.master_seconds = now_ms / 1000u,
		.fraction_65536 = (uint16_t)((now_ms % 1000u) * 65536u / 1000u),
		.cycle = a->ref_cycle,
	};
	uint8_t body[RAFT_REF_LEN];

	raft_ref_encode(&ref, body);
	send_body(a, ID_REF(a->self), body, sizeof(body));
	a->last_ref_send_ms = now_ms;
	a->ref_cycle++;
	a->ref_sent = true;
}

static void handle_client_update(struct raft_agent *a, uint8_t sender,
				 const struct raft_client_update *m)
{
	if (m->target != a->self && m->target != RAFT_TARGET_ALL) {
		return;
	}
	if (a->role != RAFT_LEADER) {
		reply_client(a, sender, m->request_id, RAFT_CLIENT_REDIRECT, 0);
		return;
	}
	/* A disconnected client may replay a stale loss after it rejoins. The
	 * leader must not record its own death or a peer it still hears alive. */
	if (m->event.type == RAFT_EVENT_NODE_DOWN && m->event.node < NUM_NODES &&
	    (m->event.node == a->self || !a->obs[1 + m->event.node].seen ||
	     a->obs[1 + m->event.node].up)) {
		client_result(a, sender, m->request_id, RAFT_CLIENT_REJECTED, 0);
		return;
	}
	/* An STM32 reports COCKPIT_ERROR when it stops hearing N1. Like NODE_DOWN,
	 * only accept it if this leader has lost N1 too. */
	if (m->event.type == RAFT_EVENT_COCKPIT_ERROR && sender != NODE_N1 &&
	    (a->self == NODE_N1 || !a->obs[1 + NODE_N1].seen || a->obs[1 + NODE_N1].up)) {
		client_result(a, sender, m->request_id, RAFT_CLIENT_REJECTED, 0);
		return;
	}
	if (a->last_client_id[sender] == m->request_id &&
	    a->last_client_index[sender] != 0) {
		client_result(a, sender, m->request_id, RAFT_CLIENT_COMMITTED,
			      a->last_client_index[sender]);
		return;
	}
	if (event_already_applied(&a->network, &m->event)) {
		a->last_client_id[sender] = m->request_id;
		a->last_client_index[sender] = a->commit_index;
		client_result(a, sender, m->request_id, RAFT_CLIENT_COMMITTED,
			      a->commit_index);
		return;
	}
	/* Another entry is in flight: stay silent, the client resends shortly. */
	if (a->pending_index) {
		return;
	}
	/* Log full (log_size reached, or no room in RAM): REJECTED makes the
	 * client wait RAFT_CLIENT_REJECT_BACKOFF_MS. */
	if (a->last_index >= RAFT_LOG_CAPACITY || !room_for(a, a->last_index + 1)) {
		client_result(a, sender, m->request_id, RAFT_CLIENT_REJECTED, 0);
		return;
	}
	entry_at(a, a->last_index + 1)->term = a->term;
	entry_at(a, a->last_index + 1)->event = m->event;
	a->last_index++;
	DBG(a, "leader: #%u term %u %s N%u from N%u", (unsigned)a->last_index,
	    (unsigned)a->term, raft_event_name(m->event.type), (unsigned)m->event.node + 1,
	    (unsigned)sender + 1);
	a->pending_index = a->last_index;
	a->pending_client = sender;
	a->pending_request_id = m->request_id;
	a->pending_acks = (uint8_t)(1u << a->self);
	a->last_append_send_ms = 0;
}

void raft_agent_init(struct raft_agent *a, uint8_t self, raft_send_fn send, void *send_ctx,
		     uint32_t now_ms, uint32_t random_seed)
{
	memset(a, 0, sizeof(*a));
	a->self = self;
	a->leader = RAFT_DEFAULT_LEADER;
	a->term = RAFT_DEFAULT_LEADER >= 0 ? 1 : 0;
	a->role = RAFT_FOLLOWER;
	a->voted_for = RAFT_DEFAULT_LEADER;
	a->send = send;
	a->send_ctx = send_ctx;
	a->base_hash = RAFT_HASH_INIT;
	a->applied_hash = RAFT_HASH_INIT;
	if (RAFT_ELECTION_SEED != 0) {
		/* Reproducible: each node's timeouts depend only on the seed. */
		a->election_random_state = (uint32_t)RAFT_ELECTION_SEED ^
					   (0x9e3779b9u * ((uint32_t)self + 1u));
	} else {
		a->election_random_state = random_seed ? random_seed : 0x6d2b79f5u ^ (uint32_t)self;
	}
	if (a->election_random_state == 0) {
		a->election_random_state = 1; /* xorshift never leaves 0 */
	}
	const int default_leader = RAFT_DEFAULT_LEADER; /* -1 in random mode */

	if (default_leader >= 0 && (int)self == default_leader) {
		/* Listen before taking term 1: a running cluster means we are a
		 * returning node (the Pi after an STM32 took over), not a fresh boot. */
		a->leader = -1;
		a->term = 0;
		a->voted_for = -1;
		a->boot_listen = true;
		a->boot_listen_until_ms = now_ms + RAFT_BOOT_LISTEN_MS;
	}
	/* With a configured leader, followers wait for its first REF before treating
	 * it as failed. In random mode the boot election waits for every node. */
	a->wait_cluster = RAFT_DEFAULT_LEADER < 0;
}

bool raft_client_report(struct raft_agent *a, struct raft_event event)
{
	bool up;
	int subject = event_subject(&event, &up);

	if (subject >= 0) {
		for (uint8_t i = 0; i < a->queue_count; i++) {
			struct raft_event *q =
				&a->queued[(a->queue_head + i) % RAFT_CLIENT_QUEUE_LEN];
			bool q_up;

			if (event_subject(q, &q_up) == subject) {
				*q = event; /* newest state wins */
				return true;
			}
		}
	}
	if (a->queue_count == RAFT_CLIENT_QUEUE_LEN) {
		return false;
	}
	uint8_t tail = (uint8_t)((a->queue_head + a->queue_count) % RAFT_CLIENT_QUEUE_LEN);
	a->queued[tail] = event;
	a->queue_count++;
	return true;
}

bool raft_client_observe(struct raft_agent *a, struct raft_event event, uint32_t now_ms)
{
	bool up;
	int subject = event_subject(&event, &up);

	if (subject < 0) {
		return raft_client_report(a, event);
	}
	struct raft_observation *o = &a->obs[subject];

	if (!o->seen || o->up != up) {
		o->seen = true;
		o->up = up;
		o->since_ms = now_ms;
		/* Do not retry a peer-loss claim after hearing that peer again. */
		if (up && a->active_request) {
			bool active_up;
			if (event_subject(&a->active_event, &active_up) == subject &&
			    !active_up) {
				a->active_request = false;
			}
		}
	}
	return true;
}

void raft_client_forget(struct raft_agent *a, struct raft_event event)
{
	bool up;
	int subject = event_subject(&event, &up);

	if (subject < 0) {
		return;
	}
	a->obs[subject] = (struct raft_observation){0};
	for (uint8_t i = 0; i < a->queue_count; i++) {
		uint8_t at = (uint8_t)((a->queue_head + i) % RAFT_CLIENT_QUEUE_LEN);
		bool q_up;

		if (event_subject(&a->queued[at], &q_up) == subject) {
			/* Shift the rest of the queue down over it. */
			for (uint8_t j = i; j + 1 < a->queue_count; j++) {
				a->queued[(a->queue_head + j) % RAFT_CLIENT_QUEUE_LEN] =
					a->queued[(a->queue_head + j + 1) % RAFT_CLIENT_QUEUE_LEN];
			}
			a->queue_count--;
			break;
		}
	}
	if (a->active_request) {
		bool active_up;

		if (event_subject(&a->active_event, &active_up) == subject) {
			a->active_request = false;
		}
	}
}

/* Queue each observation that has held long enough and differs from the last report. */
static void client_debounce(struct raft_agent *a, uint32_t now_ms)
{
	for (int s = 0; s < RAFT_OBS_SUBJECTS; s++) {
		struct raft_observation *o = &a->obs[s];
		uint32_t hold = o->up ? RAFT_REPORT_UP_HOLD_MS : RAFT_REPORT_DOWN_HOLD_MS;

		struct raft_event current = subject_event(s, o->up);
		bool graph_disagrees = false;
		/* Only a node can repair its own links. A cached observation about
		 * another node is not authoritative after a partition. */
		if (s > NUM_NODES) {
			int link = s - 1 - NUM_NODES;
			int node = link / 2, bus = link % 2;
			graph_disagrees = node == a->self && a->network.link_known[node][bus] &&
				a->network.link_up[node][bus] != o->up;
		}
		/* The leader already answered COMMITTED for this subject, so its
		 * log has the entry and this node simply has not applied it yet
		 * (for example a lost APPEND fragment). Re-reporting at once only
		 * loads the bus: N3 re-sent CANLINK_DOWN ~10 times a second. */
		if (graph_disagrees && o->committed_index &&
		    a->applied_index < o->committed_index &&
		    (uint32_t)(now_ms - o->committed_ms) < RAFT_CLIENT_REPAIR_HOLD_MS) {
			graph_disagrees = false;
		}
		bool in_flight = false;
		if (a->active_request) {
			bool active_up;
			in_flight = event_subject(&a->active_event, &active_up) == s &&
				active_up == o->up;
		}
		if (!o->seen ||
		    ((o->reported && o->reported_up == o->up && !graph_disagrees) || in_flight) ||
		    (uint32_t)(now_ms - o->since_ms) < hold) {
			continue;
		}
		if (raft_client_report(a, current)) {
			o->reported = true;
			o->reported_up = o->up;
		}
	}
}

void raft_client_get_network(const struct raft_agent *a, struct raft_network *out)
{
	*out = a->network;
}

static bool candidate_log_current(const struct raft_agent *a, uint32_t last_index,
				  uint32_t last_term)
{
	uint32_t own_last_term = term_at(a, a->last_index);

	return last_term > own_last_term ||
	       (last_term == own_last_term && last_index >= a->last_index);
}

static void start_pre_vote(struct raft_agent *a)
{
	struct raft_vote_request m = {
		.term = a->term + 1,
		.last_index = a->last_index,
		.last_term = term_at(a, a->last_index),
	};
	uint8_t body[RAFT_VOTE_REQUEST_LEN];

	a->pre_vote_active = true;
	a->pre_votes = (uint8_t)(1u << a->self);
	DBG(a, "election: pre-vote for term %u", (unsigned)(a->term + 1));
	raft_vote_request_encode(&m, body);
	send_body(a, ID_PRE_VOTE(a->self), body, sizeof(body));
}

void raft_agent_start_election(struct raft_agent *a)
{
	a->pre_vote_active = false;
	a->pre_votes = 0;
	a->term++;
	a->leader = -1;
	a->role = RAFT_CANDIDATE;
	a->voted_for = a->self;
	a->votes = (uint8_t)(1u << a->self);
	DBG(a, "election: candidate, term %u, requesting votes", (unsigned)a->term);
	struct raft_vote_request m = {
		.term = a->term,
		.last_index = a->last_index,
		.last_term = term_at(a, a->last_index),
	};
	uint8_t body[RAFT_VOTE_REQUEST_LEN];

	raft_vote_request_encode(&m, body);
	send_body(a, ID_REQ_VOTE(a->self), body, sizeof(body));
}

static void handle_append(struct raft_agent *a, uint8_t sender,
			  const struct raft_append *m, uint32_t now_ms)
{
	struct raft_append_ack ack = {
		.term = a->term, .match_index = a->last_index + 1,
		.target = sender, .success = 0,
	};
	uint8_t body[RAFT_APPEND_ACK_LEN];

	if (m->target != a->self && m->target != RAFT_TARGET_ALL) {
		return;
	}
	if (m->term >= a->term &&
	    (m->term > a->term || a->leader < 0 || a->leader == sender)) {
		bool new_leader = a->leader != sender || a->role != RAFT_FOLLOWER ||
				  m->term != a->term;

		if (m->term > a->term) {
			a->voted_for = -1;
		}
		if (new_leader) {
			DBG(a, "follower: leader N%u term %u%s", (unsigned)sender + 1,
			    (unsigned)m->term, a->role == RAFT_LEADER ? " (stepping down)" : "");
		}
		a->term = m->term;
		a->leader = sender;
		a->role = RAFT_FOLLOWER;
		a->pre_vote_active = false;
		a->boot_listen = false;  /* a running cluster: join it */
		a->wait_cluster = false;
		if (new_leader) {
			reset_election_timer(a, now_ms);
		}
		ack.term = a->term;
		uint32_t index = m->prev_index + 1;

		if (m->entry_count == RAFT_SNAPSHOT_MARK) {
			if (m->prev_index > a->applied_index) {
				/* InstallSnapshot: everything through prev_index is committed;
				 * take the leader's state and hash, drop our log. */
				a->log_base = m->prev_index;
				a->base_term = m->prev_term;
				raft_network_unpack(m->snapshot_network, &a->base_network);
				a->network = a->base_network;
				a->base_hash = m->snapshot_hash;
				a->applied_hash = m->snapshot_hash;
				a->last_index = m->prev_index;
				a->commit_index = m->prev_index;
				a->applied_index = m->prev_index;
				if (a->replay_boundary < m->prev_index) {
					a->replay_boundary = m->prev_index;
				}
				DBG(a, "follower: snapshot through #%u from N%u", (unsigned)m->prev_index,
				    (unsigned)sender + 1);
				if (a->on_apply) {
					a->on_apply(a->apply_ctx, m->prev_index, NULL, true, &a->network);
				}
			}
			ack.success = 1;
			ack.match_index = m->prev_index;
		} else if (m->prev_index <= a->last_index &&
			   prev_matches(a, m->prev_index, m->prev_term)) {
			if (m->entry_count == 0) {
				ack.success = 1;
				ack.match_index = m->prev_index;
			} else if (index <= a->log_base) {
				/* Compacted, so committed: it is the leader's entry. */
				ack.success = 1;
				ack.match_index = index;
			} else if (index <= a->last_index && term_at(a, index) == m->entry_term) {
				ack.success = 1; /* already have it */
				ack.match_index = index;
			} else if (index > a->commit_index) {
				if (index <= a->last_index) {
					a->last_index = index - 1; /* drop a conflicting suffix */
				}
				if (room_for(a, index)) {
					entry_at(a, index)->term = m->entry_term;
					entry_at(a, index)->event = m->entry;
					a->last_index = index;
					/* Already committed in the cluster before we got it: a
					 * catch-up replay rather than a live entry. */
					if (m->leader_commit >= index && a->replay_boundary < index) {
						a->replay_boundary = index;
					}
					ack.success = 1;
					ack.match_index = index;
					DBG(a, "follower: #%u term %u %s from N%u", (unsigned)index,
					    (unsigned)m->entry_term, raft_event_name(m->entry.type),
					    (unsigned)sender + 1);
				}
			}
			if (ack.success && m->leader_commit > a->commit_index &&
			    ack.match_index > a->commit_index) {
				/* Only the prefix verified by this APPEND can be committed. */
				a->commit_index = m->leader_commit < ack.match_index ?
						  m->leader_commit : ack.match_index;
				apply_committed(a);
			}
		} else if (m->prev_index <= a->last_index) {
			/* The preceding entry exists but conflicts. Retry from that index. */
			ack.match_index = m->prev_index;
		}
	}
	raft_append_ack_encode(&ack, body);
	send_body(a, ID_APPEND_ACK(a->self), body, sizeof(body));
}

static void handle_append_ack(struct raft_agent *a, uint8_t sender,
				const struct raft_append_ack *m, uint32_t now_ms)
{
	if (m->term > a->term) {
		a->term = m->term;
		a->role = RAFT_FOLLOWER;
		a->leader = -1;
		a->voted_for = -1;
		a->pre_vote_active = false;
		reset_election_timer(a, now_ms);
		return;
	}
	if (a->role != RAFT_LEADER || m->target != a->self || m->term != a->term) {
		return;
	}
	if (!m->success) {
		if (m->match_index > 0 && m->match_index <= a->last_index) {
			/* A restarted follower may have lost progress we recorded earlier. */
			a->follower_match[sender] = m->match_index - 1;
			send_append(a, sender, m->match_index);
		}
		return;
	}
	if (m->match_index > a->last_index) {
		return;
	}
	if (m->match_index > a->follower_match[sender]) {
		a->follower_match[sender] = m->match_index;
		if (m->match_index < a->last_index) {
			/* Continue catch-up one entry at a time, including committed entries. */
			send_append(a, sender, m->match_index + 1);
		}
	}
	if (a->pending_index == 0 || m->match_index != a->pending_index) {
		return;
	}
	a->pending_acks |= (uint8_t)(1u << sender);
	if (__builtin_popcount((unsigned)a->pending_acks) < RAFT_CLUSTER_SIZE / 2 + 1) {
		return;
	}
	a->commit_index = a->pending_index;
	DBG(a, "leader: commit #%u (majority %02x)", (unsigned)a->commit_index,
	    (unsigned)a->pending_acks);
	apply_committed(a);
	a->last_client_id[a->pending_client] = a->pending_request_id;
	a->last_client_index[a->pending_client] = a->commit_index;
	client_result(a, a->pending_client, a->pending_request_id,
		      RAFT_CLIENT_COMMITTED, a->commit_index);
	a->pending_index = 0;
	send_empty_append(a);
}

void raft_agent_receive(struct raft_agent *a, uint32_t id, const uint8_t *body,
			uint8_t len, uint32_t now_ms)
{
	int sender = catalog_sender(id);
	uint32_t kind = id & ~3u;

	if (!catalog_is_raft(id) || sender < 0 || sender == a->self) {
		return;
	}
	if (kind == ID_REF(0) && len == RAFT_REF_LEN) {
		if (a->boot_listen) {
			/* Another node leads: wait for its APPEND (which carries the term)
			 * instead of taking term 1. */
			a->boot_listen_until_ms = now_ms + RAFT_BOOT_LISTEN_MS;
		}
		if (sender == a->leader) {
			a->last_ref_ms = now_ms;
			a->pre_vote_active = false;
			reset_election_timer(a, now_ms);
		}
	} else if (kind == ID_CLIENT_UPDATE(0) && len == RAFT_CLIENT_UPDATE_LEN) {
		struct raft_client_update m;

		raft_client_update_decode(body, &m);
		handle_client_update(a, (uint8_t)sender, &m);
	} else if (kind == ID_CLIENT_REPLY(0) && len == RAFT_CLIENT_REPLY_LEN) {
		struct raft_client_reply m;

		raft_client_reply_decode(body, &m);
		if (m.target == a->self && a->active_request &&
		    m.request_id == a->active_request_id) {
			if (m.status == RAFT_CLIENT_COMMITTED) {
				bool up;
				int subject = event_subject(&a->active_event, &up);

				if (subject >= 0) {
					a->obs[subject].committed_index = m.committed_index;
					a->obs[subject].committed_ms = now_ms;
				}
				a->active_request = false;
			} else if (m.status == RAFT_CLIENT_REDIRECT && m.leader < NUM_NODES &&
				   m.leader != a->self) {
				a->leader = (int8_t)m.leader;
				a->last_client_send_ms = 0;
				a->client_retry_ms = RAFT_CLIENT_RETRY_MS;
			} else if (m.status == RAFT_CLIENT_REJECTED) {
				a->last_client_send_ms = now_ms ? now_ms : 1;
				a->client_retry_ms = RAFT_CLIENT_REJECT_BACKOFF_MS;
			}
		}
	} else if (kind == ID_APPEND(0) &&
		   (len == RAFT_APPEND_EMPTY_LEN || len == RAFT_APPEND_ENTRY_LEN)) {
		struct raft_append m;

		if (raft_append_decode(body, len, &m)) {
			handle_append(a, (uint8_t)sender, &m, now_ms);
		}
	} else if (kind == ID_APPEND_ACK(0) && len == RAFT_APPEND_ACK_LEN) {
		struct raft_append_ack m;

		raft_append_ack_decode(body, &m);
		handle_append_ack(a, (uint8_t)sender, &m, now_ms);
	} else if (kind == ID_PRE_VOTE(0) && len == RAFT_VOTE_REQUEST_LEN) {
		struct raft_vote_request m;
		struct raft_vote_reply reply;
		uint8_t encoded[RAFT_VOTE_REPLY_LEN];

		raft_vote_request_decode(body, &m);
		/* A current leader or a recently heard leader prevents an election. */
		bool leader_recent = a->role == RAFT_LEADER ||
			(a->leader >= 0 &&
			 (uint32_t)(now_ms - a->last_ref_ms) <=
			 RAFT_MISSED_HEARTBEATS * RAFT_HEARTBEAT_MS +
			 RAFT_ELECTION_JITTER_MS);

		reply.term = m.term;
		reply.candidate = (uint8_t)sender;
		reply.granted = !leader_recent && m.term > a->term &&
			candidate_log_current(a, m.last_index, m.last_term);
		raft_vote_reply_encode(&reply, encoded);
		send_body(a, ID_PRE_VOTE_REPLY(a->self), encoded, sizeof(encoded));
	} else if (kind == ID_PRE_VOTE_REPLY(0) && len == RAFT_VOTE_REPLY_LEN) {
		struct raft_vote_reply m;

		raft_vote_reply_decode(body, &m);
		if (a->pre_vote_active && m.candidate == a->self &&
		    m.term == a->term + 1 && m.granted) {
			a->pre_votes |= (uint8_t)(1u << sender);
			if (__builtin_popcount((unsigned)a->pre_votes) >=
			    RAFT_CLUSTER_SIZE / 2 + 1) {
				raft_agent_start_election(a);
				reset_election_timer(a, now_ms);
			}
		}
	} else if (kind == ID_REQ_VOTE(0) && len == RAFT_VOTE_REQUEST_LEN) {
		struct raft_vote_request m;
		struct raft_vote_reply reply;
		uint8_t encoded[RAFT_VOTE_REPLY_LEN];

		raft_vote_request_decode(body, &m);
		bool log_current = candidate_log_current(a, m.last_index, m.last_term);
		if (m.term > a->term) {
			a->term = m.term;
			a->role = RAFT_FOLLOWER;
			a->leader = -1;
			a->voted_for = -1;
			a->pre_vote_active = false;
			reset_election_timer(a, now_ms);
		}
		reply.term = a->term;
		reply.candidate = (uint8_t)sender;
		reply.granted = m.term == a->term && log_current &&
			(a->voted_for < 0 || a->voted_for == sender);
		DBG(a, "election: vote for N%u term %u %s", (unsigned)sender + 1, (unsigned)m.term,
		    reply.granted ? "granted" : "denied");
		if (reply.granted) {
			a->voted_for = (int8_t)sender;
			reset_election_timer(a, now_ms);
		}
		raft_vote_reply_encode(&reply, encoded);
		send_body(a, ID_VOTE_REPLY(a->self), encoded, sizeof(encoded));
	} else if (kind == ID_VOTE_REPLY(0) && len == RAFT_VOTE_REPLY_LEN) {
		struct raft_vote_reply m;

		raft_vote_reply_decode(body, &m);
		if (m.term > a->term) {
			a->term = m.term;
			a->role = RAFT_FOLLOWER;
			a->leader = -1;
			a->voted_for = -1;
			a->pre_vote_active = false;
			reset_election_timer(a, now_ms);
			return;
		}
		if (a->role == RAFT_CANDIDATE && m.term == a->term &&
		    m.candidate == a->self && m.granted) {
			a->votes |= (uint8_t)(1u << sender);
			if (__builtin_popcount((unsigned)a->votes) >= RAFT_CLUSTER_SIZE / 2 + 1) {
				a->role = RAFT_LEADER;
				a->leader = a->self;
				a->election_timer_active = false;
				memset(a->follower_match, 0, sizeof(a->follower_match));
				DBG(a, "election: leader for term %u (votes %02x)", (unsigned)a->term,
				    (unsigned)a->votes);
			}
		}
	}
}

void raft_agent_poll(struct raft_agent *a, uint32_t now_ms)
{
	if (a->boot_listen && (int32_t)(now_ms - a->boot_listen_until_ms) >= 0) {
		/* Nobody else is leading: a fresh cluster, take the configured term 1. */
		a->boot_listen = false;
		a->boot_led = true;
		a->term = 1;
		a->role = RAFT_LEADER;
		a->leader = a->self;
		a->voted_for = a->self;
		DBG(a, "boot: no cluster heard in %u ms, default leader for term 1",
		    (unsigned)RAFT_BOOT_LISTEN_MS);
	}
	if (a->wait_cluster) {
		bool all = true;

		for (int n = 0; n < NUM_NODES; n++) {
			if (n != a->self && !(a->obs[1 + n].seen && a->obs[1 + n].up)) {
				all = false;
			}
		}
		if (all) {
			/* Every node is up: start the boot election clocks together. */
			a->wait_cluster = false;
			reset_election_timer(a, now_ms);
			DBG(a, "boot: all nodes heard, election timeout %u ms",
			    (unsigned)(a->election_deadline_ms - now_ms));
		}
	}
	if (a->election_timer_active && a->role != RAFT_LEADER &&
	    (int32_t)(now_ms - a->election_deadline_ms) >= 0) {
		start_pre_vote(a);
		reset_election_timer(a, now_ms);
	}
	if (a->role == RAFT_LEADER) {
		a->election_timer_active = false;
		if (!a->ref_sent || now_ms - a->last_ref_send_ms >= RAFT_HEARTBEAT_MS) {
			send_ref(a, now_ms);
		}
		if (!a->empty_append_sent ||
		    now_ms - a->last_empty_append_ms >= RAFT_EMPTY_APPEND_MS) {
			send_empty_append(a);
			a->last_empty_append_ms = now_ms;
			a->empty_append_sent = true;
		}
	}
	client_debounce(a, now_ms);
	if (!a->active_request && a->queue_count) {
		a->active_event = a->queued[a->queue_head];
		a->queue_head = (uint8_t)((a->queue_head + 1) % RAFT_CLIENT_QUEUE_LEN);
		a->queue_count--;
		a->active_request = true;
		a->active_request_id = ++a->next_request_id;
		a->last_client_send_ms = 0;
		a->client_retry_ms = RAFT_CLIENT_RETRY_MS;
	}
	if (a->active_request && !a->boot_listen &&
	    (a->last_client_send_ms == 0 ||
	     now_ms - a->last_client_send_ms >= a->client_retry_ms)) {
		struct raft_client_update m = {
			.target = a->leader < 0 ? RAFT_TARGET_ALL : (uint8_t)a->leader,
			.request_id = a->active_request_id,
			.event = a->active_event,
		};
		if (a->role == RAFT_LEADER) {
			handle_client_update(a, a->self, &m);
		} else {
			uint8_t body[RAFT_CLIENT_UPDATE_LEN];

			raft_client_update_encode(&m, body);
			send_body(a, ID_CLIENT_UPDATE(a->self), body, sizeof(body));
		}
		a->last_client_send_ms = now_ms ? now_ms : 1;
	}
	if (a->role == RAFT_LEADER && a->pending_index &&
	    (a->last_append_send_ms == 0 || now_ms - a->last_append_send_ms >= 100)) {
		send_append(a, RAFT_TARGET_ALL, a->pending_index);
		a->last_append_send_ms = now_ms ? now_ms : 1;
	}
}

uint32_t raft_agent_next_wake_ms(const struct raft_agent *a, uint32_t now_ms)
{
	uint32_t wait_ms = RAFT_HEARTBEAT_MS;

	if (a->role == RAFT_LEADER) {
		uint32_t since_ref = now_ms - a->last_ref_send_ms;
		uint32_t since_append = now_ms - a->last_empty_append_ms;

		if (!a->ref_sent || since_ref >= RAFT_HEARTBEAT_MS ||
		    !a->empty_append_sent || since_append >= RAFT_EMPTY_APPEND_MS) {
			return 1;
		}
		uint32_t ref_wait = RAFT_HEARTBEAT_MS - since_ref;
		uint32_t append_wait = RAFT_EMPTY_APPEND_MS - since_append;

		wait_ms = ref_wait < wait_ms ? ref_wait : wait_ms;
		wait_ms = append_wait < wait_ms ? append_wait : wait_ms;
	}
	if (a->election_timer_active && a->role != RAFT_LEADER) {
		int32_t until = (int32_t)(a->election_deadline_ms - now_ms);

		if (until <= 0) {
			return 1;
		}
		wait_ms = (uint32_t)until < wait_ms ? (uint32_t)until : wait_ms;
	}
	return wait_ms;
}
