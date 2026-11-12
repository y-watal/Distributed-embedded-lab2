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
