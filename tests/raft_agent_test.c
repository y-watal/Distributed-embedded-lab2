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

int main(void)
{
	test_reject_backoff();
	test_debounce_and_coalesce();

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
	return 0;
}
