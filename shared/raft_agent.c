#include "raft_agent.h"

#include <string.h>

static bool send_body(struct raft_agent *a, uint32_t id, const uint8_t *body, uint8_t len)
{
	return a->send && a->send(a->send_ctx, id, body, len);
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

static void apply_committed(struct raft_agent *a)
{
	while (a->applied_index < a->commit_index &&
	       a->applied_index < RAFT_WORKING_LOG_ENTRIES) {
		apply_event(&a->network, &a->log[a->applied_index].event);
		a->applied_index++;
	}
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

static void send_append(struct raft_agent *a, uint8_t target, uint32_t index)
{
	if (index == 0 || index > a->last_index || index > RAFT_WORKING_LOG_ENTRIES) {
		return;
	}
	struct raft_append m = {
		.target = target,
		.term = a->term,
		.prev_index = index - 1,
		.prev_term = index > 1 ? a->log[index - 2].term : 0,
		.leader_commit = a->commit_index,
		.entry_count = 1,
		.entry_term = a->log[index - 1].term,
		.entry = a->log[index - 1].event,
	};
	uint8_t body[RAFT_APPEND_ENTRY_LEN];
	uint8_t len = raft_append_encode(&m, body);

	send_body(a, ID_APPEND(a->self), body, len);
}

static bool send_empty_append(struct raft_agent *a)
{
	struct raft_append m = {
		.target = RAFT_TARGET_ALL,
		.term = a->term,
		.prev_index = a->last_index,
		.prev_term = a->last_index ? a->log[a->last_index - 1].term : 0,
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
	if (a->last_client_id[sender] == m->request_id &&
	    a->last_client_index[sender] != 0) {
		reply_client(a, sender, m->request_id, RAFT_CLIENT_COMMITTED,
			     a->last_client_index[sender]);
		return;
	}
	if (event_already_applied(&a->network, &m->event)) {
		a->last_client_id[sender] = m->request_id;
		a->last_client_index[sender] = a->commit_index;
		if (sender == a->self) {
			a->active_request = false;
		} else {
			reply_client(a, sender, m->request_id, RAFT_CLIENT_COMMITTED,
				     a->commit_index);
		}
		return;
	}
	if (a->pending_index && a->pending_client == sender &&
	    a->pending_request_id == m->request_id) {
		return;
	}
	if (a->pending_index || a->last_index >= RAFT_WORKING_LOG_ENTRIES) {
		reply_client(a, sender, m->request_id, RAFT_CLIENT_REJECTED, 0);
		return;
	}
	a->log[a->last_index].term = a->term;
	a->log[a->last_index].event = m->event;
	a->last_index++;
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
	a->role = (int)self == RAFT_DEFAULT_LEADER ? RAFT_LEADER : RAFT_FOLLOWER;
	a->voted_for = RAFT_DEFAULT_LEADER;
	a->send = send;
	a->send_ctx = send_ctx;
	a->election_random_state = random_seed ? random_seed : 0x6d2b79f5u ^ (uint32_t)self;
	/* With a configured leader, wait for its first REF before treating it as
	 * failed. Random-leader mode has no leader to wait for at boot. */
	if (a->role != RAFT_LEADER && RAFT_DEFAULT_LEADER < 0) {
		reset_election_timer(a, now_ms);
	}
}

bool raft_client_report(struct raft_agent *a, struct raft_event event)
{
	if (a->queue_count == RAFT_CLIENT_QUEUE_LEN) {
		return false;
	}
	uint8_t tail = (uint8_t)((a->queue_head + a->queue_count) % RAFT_CLIENT_QUEUE_LEN);
	a->queued[tail] = event;
	a->queue_count++;
	return true;
}

void raft_client_get_network(const struct raft_agent *a, struct raft_network *out)
{
	*out = a->network;
}

void raft_agent_start_election(struct raft_agent *a)
{
	a->term++;
	a->leader = -1;
	a->role = RAFT_CANDIDATE;
	a->voted_for = a->self;
	a->votes = (uint8_t)(1u << a->self);
	struct raft_vote_request m = {
		.term = a->term,
		.last_index = a->last_index,
		.last_term = a->last_index ? a->log[a->last_index - 1].term : 0,
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
		a->term = m->term;
		a->leader = sender;
		a->role = RAFT_FOLLOWER;
		if (new_leader) {
			reset_election_timer(a, now_ms);
		}
		ack.term = a->term;
		if (m->prev_index <= a->last_index &&
		    (m->prev_index == 0 || a->log[m->prev_index - 1].term == m->prev_term)) {
			uint32_t index = m->prev_index + 1;

			if (m->entry_count == 0) {
				ack.success = 1;
				ack.match_index = m->prev_index;
			} else if (index <= RAFT_WORKING_LOG_ENTRIES &&
				   (index > a->commit_index || index > a->last_index)) {
				if (index <= a->last_index &&
				    a->log[index - 1].term != m->entry_term) {
					a->last_index = index - 1;
				}
				if (index > a->last_index) {
					a->log[index - 1].term = m->entry_term;
					a->log[index - 1].event = m->entry;
					a->last_index = index;
				}
				ack.success = 1;
				ack.match_index = index;
			} else if (index <= a->last_index &&
				   a->log[index - 1].term == m->entry_term) {
				ack.success = 1;
				ack.match_index = index;
			}
			if (ack.success && m->leader_commit > a->commit_index) {
				a->commit_index = m->leader_commit < a->last_index ?
						  m->leader_commit : a->last_index;
				apply_committed(a);
			}
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
		reset_election_timer(a, now_ms);
		return;
	}
	if (a->role != RAFT_LEADER || m->target != a->self || m->term != a->term) {
		return;
	}
	if (!m->success) {
		if (m->match_index > 0 && m->match_index <= a->last_index) {
			send_append(a, sender, m->match_index);
		}
		return;
	}
	if (a->pending_index == 0 || m->match_index != a->pending_index) {
		return;
	}
	a->pending_acks |= (uint8_t)(1u << sender);
	if (__builtin_popcount((unsigned)a->pending_acks) < RAFT_CLUSTER_SIZE / 2 + 1) {
		return;
	}
	a->commit_index = a->pending_index;
	apply_committed(a);
	a->last_client_id[a->pending_client] = a->pending_request_id;
	a->last_client_index[a->pending_client] = a->commit_index;
	if (a->pending_client == a->self) {
		a->active_request = false;
	} else {
		reply_client(a, a->pending_client, a->pending_request_id,
			     RAFT_CLIENT_COMMITTED, a->commit_index);
	}
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
		if (sender == a->leader) {
			a->last_ref_ms = now_ms;
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
				a->active_request = false;
			} else if (m.status == RAFT_CLIENT_REDIRECT && m.leader < NUM_NODES) {
				a->leader = (int8_t)m.leader;
				a->last_client_send_ms = 0;
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
	} else if (kind == ID_REQ_VOTE(0) && len == RAFT_VOTE_REQUEST_LEN) {
		struct raft_vote_request m;
		struct raft_vote_reply reply;
		uint8_t encoded[RAFT_VOTE_REPLY_LEN];

		raft_vote_request_decode(body, &m);
		bool log_current = m.last_term > (a->last_index ? a->log[a->last_index - 1].term : 0) ||
			(m.last_term == (a->last_index ? a->log[a->last_index - 1].term : 0) &&
			 m.last_index >= a->last_index);
		if (m.term > a->term) {
			a->term = m.term;
			a->role = RAFT_FOLLOWER;
			a->leader = -1;
			a->voted_for = -1;
			reset_election_timer(a, now_ms);
		}
		reply.term = a->term;
		reply.candidate = (uint8_t)sender;
		reply.granted = m.term == a->term && log_current &&
			(a->voted_for < 0 || a->voted_for == sender);
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
			}
		}
	}
}

void raft_agent_poll(struct raft_agent *a, uint32_t now_ms)
{
	if (a->election_timer_active && a->role != RAFT_LEADER &&
	    (int32_t)(now_ms - a->election_deadline_ms) >= 0) {
		raft_agent_start_election(a);
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
	if (!a->active_request && a->queue_count) {
		a->active_event = a->queued[a->queue_head];
		a->queue_head = (uint8_t)((a->queue_head + 1) % RAFT_CLIENT_QUEUE_LEN);
		a->queue_count--;
		a->active_request = true;
		a->active_request_id = ++a->next_request_id;
		a->last_client_send_ms = 0;
	}
	if (a->active_request &&
	    (a->last_client_send_ms == 0 || now_ms - a->last_client_send_ms >= 100)) {
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
