/*
 * Random-leader mode. Build with
 *   -DRAFT_DEFAULT_LEADER=-1 -DRAFT_ELECTION_SEED=RAFT_SEED_PI_WINS
 * No boot election runs until every node is heard, and with the Pi-wins seed
 * the cockpit (N1) times out first and is elected in term 1.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "raft_agent.h"

#define QLEN 512

struct message {
	uint32_t id;
	uint8_t len;
	uint8_t body[RAFT_MAX_BODY_LEN];
};

static struct {
	struct raft_agent nodes[NUM_NODES];
	struct message q[QLEN];
	unsigned head, count;
} s;

static bool send_message(void *ctx, uint32_t id, const uint8_t *body, uint8_t len)
{
	(void)ctx;
	assert(s.count < QLEN);
	struct message *m = &s.q[(s.head + s.count++) % QLEN];

	m->id = id;
	m->len = len;
	memcpy(m->body, body, len);
	return true;
}

static void step(uint32_t now)
{
	for (int n = 0; n < NUM_NODES; n++) {
		raft_agent_poll(&s.nodes[n], now);
	}
	while (s.count) {
		struct message m = s.q[s.head];

		s.head = (s.head + 1) % QLEN;
		s.count--;
		for (int n = 0; n < NUM_NODES; n++) {
			if (n != catalog_sender(m.id)) {
				raft_agent_receive(&s.nodes[n], m.id, m.body, m.len, now);
			}
		}
	}
}

int main(void)
{
	_Static_assert(RAFT_DEFAULT_LEADER == -1, "build with -DRAFT_DEFAULT_LEADER=-1");

	for (int n = 0; n < NUM_NODES; n++) {
		raft_agent_init(&s.nodes[n], (uint8_t)n, send_message, NULL, 0, 0);
		assert(s.nodes[n].leader == -1 && s.nodes[n].wait_cluster);
	}
	/* The STM32s boot long before the Pi: no election while a node is missing. */
	for (uint32_t t = 0; t < 2000; t += 5) {
		step(t);
	}
	for (int n = 0; n < NUM_NODES; n++) {
		assert(s.nodes[n].term == 0 && s.nodes[n].role == RAFT_FOLLOWER);
	}
	/* The Pi starts: within one heartbeat every node has heard every other. */
	for (int n = 0; n < NUM_NODES; n++) {
		for (int peer = 0; peer < NUM_NODES; peer++) {
			if (peer != n) {
				raft_client_observe(&s.nodes[n], (struct raft_event){
					RAFT_EVENT_NODE_UP, (uint8_t)peer, RAFT_BUS_NONE, 0},
					2000 + (uint32_t)(n * 7)); /* up to ~20 ms skew */
			}
		}
	}
	int leader = -1;

	for (uint32_t t = 2000; t < 3000 && leader < 0; t++) {
		step(t);
		for (int n = 0; n < NUM_NODES; n++) {
			if (s.nodes[n].role == RAFT_LEADER) {
				leader = n;
			}
		}
	}
	printf("random mode, seed %u: leader N%d term %u\n", (unsigned)RAFT_ELECTION_SEED,
	       leader + 1, (unsigned)s.nodes[leader].term);
	if (RAFT_ELECTION_SEED == RAFT_SEED_PI_WINS) {
		assert(leader == NODE_N1 && s.nodes[NODE_N1].term == 1);
	}
	assert(leader >= 0);
	return 0;
}
