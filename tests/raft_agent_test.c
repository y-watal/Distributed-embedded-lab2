#include <assert.h>
#include <string.h>

#include "raft_agent.h"

#define MESSAGE_QUEUE_LEN 256

struct message {
	uint32_t id;
	uint8_t len;
	uint8_t body[RAFT_MAX_BODY_LEN];
};

struct simulation {
	struct raft_agent nodes[NUM_NODES];
	bool active[NUM_NODES];
	struct message messages[MESSAGE_QUEUE_LEN];
	unsigned head, count;
};

static bool send_message(void *ctx, uint32_t id, const uint8_t *body, uint8_t len)
{
	struct simulation *s = ctx;

	assert(s->count < MESSAGE_QUEUE_LEN);
	struct message *m = &s->messages[(s->head + s->count++) % MESSAGE_QUEUE_LEN];

	m->id = id;
	m->len = len;
	memcpy(m->body, body, len);
	return true;
}

/* Standalone agent sink: counts what one node would put on the bus. */
struct sink {
	unsigned updates, replies;
	struct raft_client_reply last_reply;
};

static bool sink_send(void *ctx, uint32_t id, const uint8_t *body, uint8_t len)
{
	struct sink *k = ctx;

	(void)len;
	if ((id & ~3u) == ID_CLIENT_UPDATE(0)) {
		k->updates++;
	} else if ((id & ~3u) == ID_CLIENT_REPLY(0)) {
		k->replies++;
		raft_client_reply_decode(body, &k->last_reply);
	}
	return true;
}

static void deliver_update(struct raft_agent *leader, uint8_t from, uint32_t request_id,
			   struct raft_event e, uint32_t now)
{
	struct raft_client_update m = {.target = leader->self, .request_id = request_id, .event = e};
	uint8_t body[RAFT_CLIENT_UPDATE_LEN];

	raft_client_update_encode(&m, body);
	raft_agent_receive(leader, ID_CLIENT_UPDATE(from), body, sizeof(body), now);
}

static void test_commit_stops_at_matching_prefix(void)
{
	struct sink k = {0};
	struct raft_agent follower;
	struct raft_append append = {
		.target = NODE_N3, .term = 2, .prev_index = 0, .prev_term = 0,
		.leader_commit = 2, .entry_count = 1, .entry_term = 1,
		.entry = {RAFT_EVENT_NODE_UP, NODE_N2, RAFT_BUS_NONE, 0},
	};
	uint8_t body[RAFT_APPEND_ENTRY_LEN];

	raft_agent_init(&follower, NODE_N3, sink_send, &k, 0, 3);
	follower.last_index = 2;
	follower.log[0].term = 1;
	follower.log[0].event = append.entry;
	follower.log[1].term = 1;
	follower.log[1].event = (struct raft_event){RAFT_EVENT_CANLINK_DOWN, NODE_N3, 0, 0};
	uint8_t len = raft_append_encode(&append, body);

	raft_agent_receive(&follower, ID_APPEND(NODE_N1), body, len, 10);
	assert(follower.commit_index == 1 && follower.applied_index == 1);
	assert(!follower.network.link_known[NODE_N3][0]);

	append.prev_index = 1;
	append.prev_term = 1;
	append.entry_term = 2;
	append.entry = (struct raft_event){RAFT_EVENT_CANLINK_UP, NODE_N2, 0, 0};
	len = raft_append_encode(&append, body);
	raft_agent_receive(&follower, ID_APPEND(NODE_N1), body, len, 11);
	assert(follower.commit_index == 2 && follower.applied_index == 2);
	assert(follower.log[1].term == 2 &&
	       follower.log[1].event.type == RAFT_EVENT_CANLINK_UP &&
	       follower.network.link_up[NODE_N2][0] &&
	       !follower.network.link_known[NODE_N3][0]);
}

static void test_reject_backoff(void)
{
	struct sink lk = {0}, ck = {0};
	struct raft_agent leader, client;
	const struct raft_event e = {RAFT_EVENT_CANLINK_DOWN, NODE_N2, 0, 0};

	raft_agent_init(&leader, NODE_N1, sink_send, &lk, 0, 1);
	raft_agent_init(&client, NODE_N2, sink_send, &ck, 0, 2);
	leader.last_index = RAFT_WORKING_LOG_ENTRIES; /* full working log */

	/* Remote client: one REJECTED, then one retry per second, not ten. */
	assert(raft_client_report(&client, e));
	raft_agent_poll(&client, 1);
	assert(ck.updates == 1);
	deliver_update(&leader, NODE_N2, client.active_request_id, e, 2);
	assert(lk.replies == 1 && lk.last_reply.status == RAFT_CLIENT_REJECTED);
	uint8_t body[RAFT_CLIENT_REPLY_LEN];
	raft_client_reply_encode(&lk.last_reply, body);
	raft_agent_receive(&client, ID_CLIENT_REPLY(NODE_N1), body, sizeof(body), 3);
	for (uint32_t t = 5; t < 1003; t += 5) {
		raft_agent_poll(&client, t);
	}
	assert(ck.updates == 1);
	raft_agent_poll(&client, 1003);
	assert(ck.updates == 2);

	/* The leader's own request is answered locally: no CAN reply to itself. */
	lk.replies = 0;
	assert(raft_client_report(&leader, (struct raft_event){RAFT_EVENT_CANLINK_DOWN,
								NODE_N1, 1, 0}));
	for (uint32_t t = 10; t < 1000; t += 5) {
		raft_agent_poll(&leader, t);
	}
	assert(lk.replies == 0 && lk.updates == 0);
	assert(leader.active_request && leader.client_retry_ms == RAFT_CLIENT_REJECT_BACKOFF_MS);
}

static void test_debounce_and_coalesce(void)
{
	struct sink k = {0};
	struct raft_agent a;
	const struct raft_event up = {RAFT_EVENT_CANLINK_UP, NODE_N2, 0, 0};
	const struct raft_event down = {RAFT_EVENT_CANLINK_DOWN, NODE_N2, 0, 0};
	uint32_t t;

	raft_agent_init(&a, NODE_N2, sink_send, &k, 0, 3);

	/* The first observation is reported once it has held. */
	raft_client_observe(&a, up, 0);
	for (t = 0; t < RAFT_REPORT_UP_HOLD_MS; t += 5) {
		raft_agent_poll(&a, t);
		assert(!a.active_request);
	}
	raft_agent_poll(&a, t);
	assert(a.active_request && a.active_event.type == RAFT_EVENT_CANLINK_UP);
	a.active_request = false; /* as if committed */

	/* A 40 ms DEG flicker never reaches the log. */
	raft_client_observe(&a, down, 200);
	raft_client_observe(&a, up, 240);
	for (t = 200; t <= 600; t += 5) {
		raft_agent_poll(&a, t);
	}
	assert(!a.active_request && a.queue_count == 0);

	/* A real loss is reported after the down hold. */
	raft_client_observe(&a, down, 700);
	raft_agent_poll(&a, 700 + RAFT_REPORT_DOWN_HOLD_MS - 5);
	assert(!a.active_request);
	raft_agent_poll(&a, 700 + RAFT_REPORT_DOWN_HOLD_MS);
	assert(a.active_request && a.active_event.type == RAFT_EVENT_CANLINK_DOWN);

	/* While that is in flight, newer states of one link replace each other. */
	assert(raft_client_report(&a, up));
	assert(raft_client_report(&a, down));
	assert(raft_client_report(&a, (struct raft_event){RAFT_EVENT_NODE_DOWN, NODE_N3,
							   RAFT_BUS_NONE, 0}));
	assert(a.queue_count == 2);
	assert(a.queued[a.queue_head].type == RAFT_EVENT_CANLINK_DOWN);
}

static void test_stale_peer_loss_is_rejected(void)
{
	struct sink sink = {0};
	struct raft_agent leader;
	struct raft_event down = {RAFT_EVENT_NODE_DOWN, NODE_N2, RAFT_BUS_NONE, 0};

	raft_agent_init(&leader, NODE_N1, sink_send, &sink, 0, 1);
	raft_client_observe(&leader,
		(struct raft_event){RAFT_EVENT_NODE_UP, NODE_N2, RAFT_BUS_NONE, 0}, 1);
	deliver_update(&leader, NODE_N3, 7, down, 2);
	assert(leader.last_index == 0 && leader.pending_index == 0);
	assert(sink.replies == 1 && sink.last_reply.status == RAFT_CLIENT_REJECTED);

	down.node = NODE_N1;
	deliver_update(&leader, NODE_N3, 8, down, 3);
	assert(leader.last_index == 0 && sink.replies == 2 &&
	       sink.last_reply.status == RAFT_CLIENT_REJECTED);

	/* A peer loss can be logged once the leader also observes that loss. */
	down.node = NODE_N2;
	raft_client_observe(&leader, down, 4);
	deliver_update(&leader, NODE_N3, 7, down, 5);
	assert(leader.last_index == 1 && leader.pending_index == 1);
}

static void test_recovery_cancels_stale_loss_and_repairs_graph(void)
{
	struct sink sink = {0};
	struct raft_agent client;
	struct raft_event down = {RAFT_EVENT_NODE_DOWN, NODE_N1, RAFT_BUS_NONE, 0};
	struct raft_event up = {RAFT_EVENT_NODE_UP, NODE_N1, RAFT_BUS_NONE, 0};

	raft_agent_init(&client, NODE_N3, sink_send, &sink, 0, 3);
	raft_client_observe(&client, down, 1);
	raft_agent_poll(&client, 1 + RAFT_REPORT_DOWN_HOLD_MS);
	assert(client.active_request && client.active_event.type == RAFT_EVENT_NODE_DOWN);
	raft_client_observe(&client, up, 60);
	assert(!client.active_request);
	raft_agent_poll(&client, 60 + RAFT_REPORT_UP_HOLD_MS);
	assert(client.active_request && client.active_event.type == RAFT_EVENT_NODE_UP);

	/* A committed NODE_DOWN also marked both links down. Stable local
	 * link-up observations must restore them without a new physical edge. */
	client.active_request = false;
	raft_client_observe(&client,
		(struct raft_event){RAFT_EVENT_CANLINK_UP, NODE_N3, 0, 0}, 200);
	raft_agent_poll(&client, 200 + RAFT_REPORT_UP_HOLD_MS);
	assert(client.active_request && client.active_event.type == RAFT_EVENT_CANLINK_UP);
	client.active_request = false;
	client.network.link_known[NODE_N3][0] = true;
	client.network.link_up[NODE_N3][0] = false;
	raft_agent_poll(&client, 301);
	assert(client.active_request && client.active_event.type == RAFT_EVENT_CANLINK_UP);
}

static void pump(struct simulation *s, uint32_t now)
{
	for (unsigned rounds = 0; rounds < 100 && s->count; rounds++) {
		struct message m = s->messages[s->head];
		s->head = (s->head + 1) % MESSAGE_QUEUE_LEN;
		s->count--;
		int sender = catalog_sender(m.id);

		for (int node = 0; node < NUM_NODES; node++) {
			if (s->active[sender] && s->active[node] && node != sender) {
				raft_agent_receive(&s->nodes[node], m.id, m.body, m.len, now);
			}
		}
		for (int node = 0; node < NUM_NODES; node++) {
			if (s->active[node]) {
				raft_agent_poll(&s->nodes[node], now);
			}
		}
	}
	assert(s->count == 0);
}

static void assert_same_committed_log(const struct raft_agent *leader,
				      const struct raft_agent *follower)
{
	assert(follower->last_index == leader->last_index);
	assert(follower->commit_index == leader->commit_index);
	assert(follower->applied_index == leader->applied_index);
	for (uint32_t i = 0; i < leader->commit_index; i++) {
		assert(follower->log[i].term == leader->log[i].term);
		assert(memcmp(&follower->log[i].event, &leader->log[i].event,
			      sizeof(struct raft_event)) == 0);
	}
}

static void test_returning_follower_catches_up(void)
{
	struct simulation s = {0};
	const struct raft_event events[] = {
		{RAFT_EVENT_NODE_UP, NODE_N2, RAFT_BUS_NONE, 0},
		{RAFT_EVENT_CANLINK_UP, NODE_N2, 0, 0},
		{RAFT_EVENT_CANLINK_UP, NODE_N2, 1, 0},
	};

	for (int node = 0; node < NUM_NODES; node++) {
		raft_agent_init(&s.nodes[node], (uint8_t)node, send_message, &s, 0,
				(uint32_t)(node + 1));
	}
	s.active[NODE_N1] = true;
	s.active[NODE_N2] = true;
	/* N3 misses three entries while N1 and N2 form a majority. */
	for (uint32_t i = 0; i < sizeof(events) / sizeof(events[0]); i++) {
		uint32_t now = 10 + i * 10;

		deliver_update(&s.nodes[NODE_N1], NODE_N2, i + 1, events[i], now);
		raft_agent_poll(&s.nodes[NODE_N1], now);
		pump(&s, now);
		assert(s.nodes[NODE_N1].commit_index == i + 1);
	}
	assert(s.nodes[NODE_N3].last_index == 0);
	s.active[NODE_N3] = true;
	/* The next required empty APPEND discovers the missing prefix. */
	raft_agent_poll(&s.nodes[NODE_N1], 110);
	pump(&s, 110);
	assert_same_committed_log(&s.nodes[NODE_N1], &s.nodes[NODE_N2]);
	assert_same_committed_log(&s.nodes[NODE_N1], &s.nodes[NODE_N3]);

	/* A reboot loses N3's RAM log and must trigger the same catch-up again. */
	raft_agent_init(&s.nodes[NODE_N3], NODE_N3, send_message, &s, 120, 3);
	raft_agent_poll(&s.nodes[NODE_N1], 220);
	pump(&s, 220);
	assert_same_committed_log(&s.nodes[NODE_N1], &s.nodes[NODE_N3]);
	/* Repeated empty APPEND traffic must not duplicate any entries. */
	raft_agent_poll(&s.nodes[NODE_N1], 320);
	pump(&s, 320);
	assert_same_committed_log(&s.nodes[NODE_N1], &s.nodes[NODE_N3]);
}

static void test_isolated_follower_does_not_raise_term(void)
{
	struct simulation s = {0};

	for (int node = 0; node < NUM_NODES; node++) {
		s.active[node] = true;
		raft_agent_init(&s.nodes[node], (uint8_t)node, send_message, &s, 0,
				(uint32_t)(200 + node * 17));
	}
	for (uint32_t t = 0; t <= 100; t += 5) {
		for (int node = 0; node < NUM_NODES; node++) {
			raft_agent_poll(&s.nodes[node], t);
		}
		pump(&s, t);
	}
	s.active[NODE_N3] = false;
	for (uint32_t t = 105; t <= 1000; t += 5) {
		if (t == 300) {
			raft_client_observe(&s.nodes[NODE_N1],
				(struct raft_event){RAFT_EVENT_NODE_DOWN, NODE_N3,
					RAFT_BUS_NONE, 0}, t);
			deliver_update(&s.nodes[NODE_N1], NODE_N2, 1,
				       (struct raft_event){RAFT_EVENT_NODE_DOWN, NODE_N3,
							   RAFT_BUS_NONE, 0}, t);
		}
		for (int node = 0; node < NUM_NODES; node++) {
			raft_agent_poll(&s.nodes[node], t);
		}
		pump(&s, t);
	}
	assert(s.nodes[NODE_N3].pre_vote_active);
	assert(s.nodes[NODE_N1].commit_index == 1 && s.nodes[NODE_N3].last_index == 0);
	for (int node = 0; node < NUM_NODES; node++) {
		assert(s.nodes[node].term == 1 && s.nodes[node].leader == NODE_N1);
	}
	s.active[NODE_N3] = true;
	for (uint32_t t = 1005; t <= 1250; t += 5) {
		for (int node = 0; node < NUM_NODES; node++) {
			raft_agent_poll(&s.nodes[node], t);
		}
		pump(&s, t);
	}
	for (int node = 0; node < NUM_NODES; node++) {
		assert(s.nodes[node].term == 1 && s.nodes[node].leader == NODE_N1);
	}
	assert(!s.nodes[NODE_N3].pre_vote_active);
	assert_same_committed_log(&s.nodes[NODE_N1], &s.nodes[NODE_N3]);
}

int main(void)
{
	test_commit_stops_at_matching_prefix();
	test_reject_backoff();
	test_debounce_and_coalesce();
	test_stale_peer_loss_is_rejected();
	test_recovery_cancels_stale_loss_and_repairs_graph();
	test_returning_follower_catches_up();
	test_isolated_follower_does_not_raise_term();

	struct simulation s = {0};
	struct raft_agent early_follower;

	raft_agent_init(&early_follower, NODE_N2, send_message, &s, 0, 77);
	raft_agent_poll(&early_follower, 1000);
	assert(early_follower.term == 1 && early_follower.role == RAFT_FOLLOWER);
	assert(!early_follower.election_timer_active);

	for (int node = 0; node < NUM_NODES; node++) {
		s.active[node] = true;
		raft_agent_init(&s.nodes[node], (uint8_t)node, send_message, &s,
				0, (uint32_t)(node + 1));
		assert(s.nodes[node].term == 1 && s.nodes[node].leader == NODE_N1);
	}
	assert(s.nodes[NODE_N1].role == RAFT_LEADER);
	assert(s.nodes[NODE_N2].role == RAFT_FOLLOWER);

	/* N3 believes N2 is leader, so N2 must redirect the same request to N1. */
	s.nodes[NODE_N3].leader = NODE_N2;
	assert(raft_client_report(&s.nodes[NODE_N3],
		(struct raft_event){RAFT_EVENT_CANLINK_UP, NODE_N3, 0, 0}));
	raft_agent_poll(&s.nodes[NODE_N3], 10);
	pump(&s, 11);
	assert(s.nodes[NODE_N3].leader == NODE_N1);
	raft_agent_poll(&s.nodes[NODE_N3], 12);
	pump(&s, 13);
	assert(s.nodes[NODE_N1].commit_index == 1);
	assert(s.nodes[NODE_N2].commit_index == 1);
	assert(s.nodes[NODE_N3].commit_index == 1);
	for (int node = 0; node < NUM_NODES; node++) {
		struct raft_network network;

		raft_client_get_network(&s.nodes[node], &network);
		assert(network.link_known[NODE_N3][0] && network.link_up[NODE_N3][0]);
	}
	assert(!s.nodes[NODE_N3].active_request);

	/* A second observer of the same change must not create a second log entry. */
	assert(raft_client_report(&s.nodes[NODE_N2],
		(struct raft_event){RAFT_EVENT_CANLINK_UP, NODE_N3, 0, 0}));
	raft_agent_poll(&s.nodes[NODE_N2], 20);
	pump(&s, 21);
	assert(s.nodes[NODE_N1].last_index == 1);

	/* A manually requested election still uses the same vote path. */
	raft_agent_start_election(&s.nodes[NODE_N2]);
	pump(&s, 30);
	assert(s.nodes[NODE_N2].role == RAFT_LEADER);
	assert(s.nodes[NODE_N2].term == 2 && s.nodes[NODE_N2].leader == NODE_N2);

	/* One CAN link failure leaves the leader reachable, so term stays 1. */
	struct simulation f = {0};
	for (int node = 0; node < NUM_NODES; node++) {
		f.active[node] = true;
		raft_agent_init(&f.nodes[node], (uint8_t)node, send_message, &f,
				0, (uint32_t)(100 + node * 17));
	}
	for (uint32_t t = 0; t <= 200; t += 5) {
		if (t == 50) {
			assert(raft_client_report(&f.nodes[NODE_N2],
				(struct raft_event){RAFT_EVENT_CANLINK_DOWN, NODE_N2, 0, 0}));
		}
		for (int node = 0; node < NUM_NODES; node++) {
			raft_agent_poll(&f.nodes[node], t);
		}
		pump(&f, t);
	}
	for (int node = 0; node < NUM_NODES; node++) {
		assert(f.nodes[node].term == 1 && f.nodes[node].leader == NODE_N1);
	}

	/* Remove the Pi. The two STM32 nodes must elect one leader within 1 s. */
	f.active[NODE_N1] = false;
	uint32_t elected_at = 0;
	for (uint32_t t = 205; t <= 1200; t += 5) {
		for (int node = NODE_N2; node <= NODE_N3; node++) {
			raft_agent_poll(&f.nodes[node], t);
		}
		pump(&f, t);
		if (!elected_at && f.nodes[NODE_N2].leader == f.nodes[NODE_N3].leader &&
		    f.nodes[NODE_N2].term == f.nodes[NODE_N3].term &&
		    ((f.nodes[NODE_N2].role == RAFT_LEADER) ^
		     (f.nodes[NODE_N3].role == RAFT_LEADER))) {
			elected_at = t;
		}
	}
	assert(elected_at > 200 && elected_at < 1200);
	assert(f.nodes[NODE_N2].leader == f.nodes[NODE_N3].leader);
	assert(f.nodes[NODE_N2].term == f.nodes[NODE_N3].term);
	assert(f.nodes[NODE_N2].term == 2);
	/* The old leader returns in term 1 and must follow the term-2 leader. */
	f.active[NODE_N1] = true;
	for (uint32_t t = 1205; t <= 1500; t += 5) {
		for (int node = 0; node < NUM_NODES; node++) {
			raft_agent_poll(&f.nodes[node], t);
		}
		pump(&f, t);
	}
	for (int node = 0; node < NUM_NODES; node++) {
		assert(f.nodes[node].term == 2 &&
		       f.nodes[node].leader == f.nodes[NODE_N2].leader);
	}
	assert(f.nodes[NODE_N1].role == RAFT_FOLLOWER);
	return 0;
}
