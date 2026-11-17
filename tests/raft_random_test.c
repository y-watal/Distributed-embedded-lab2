/*
 * Random-leader mode. Build with
 *   -DRAFT_DEFAULT_LEADER=-1 -DRAFT_ELECTION_SEED=<seed>
 * 1. Any majority elects: with the Pi absent, N2 and N3 choose a leader, and
 *    the Pi later joins as a follower without a new term.
 * 2. Nodes that come up together start their election clocks together, so a
 *    fixed seed decides: RAFT_SEED_PI_WINS -> N1, RAFT_SEED_N2_WINS -> N2,
 *    RAFT_SEED_N3_WINS -> N3, all in term 1.
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
	bool active[NUM_NODES];
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
		if (s.active[n]) {
			raft_agent_poll(&s.nodes[n], now);
		}
	}
	while (s.count) {
		struct message m = s.q[s.head];
		int sender = catalog_sender(m.id);

		s.head = (s.head + 1) % QLEN;
		s.count--;
		for (int n = 0; n < NUM_NODES; n++) {
			if (n != sender && s.active[n] && s.active[sender]) {
				raft_agent_receive(&s.nodes[n], m.id, m.body, m.len, now);
			}
		}
	}
}

/* Node n's heartbeat monitor hears peer (NODE_UP observation). */
static void hears(int n, int peer, uint32_t now)
{
	raft_client_observe(&s.nodes[n], (struct raft_event){RAFT_EVENT_NODE_UP, (uint8_t)peer,
							     RAFT_BUS_NONE, 0}, now);
}

static int leader_of(void)
{
	for (int n = 0; n < NUM_NODES; n++) {
		if (s.active[n] && s.nodes[n].role == RAFT_LEADER) {
			return n;
		}
	}
	return -1;
}

static void init_all(void)
{
	memset(&s, 0, sizeof(s));
	for (int n = 0; n < NUM_NODES; n++) {
		raft_agent_init(&s.nodes[n], (uint8_t)n, send_message, NULL, 0, (uint32_t)n + 7);
		assert(s.nodes[n].leader == -1 && s.nodes[n].wait_cluster);
	}
}

/* 1. The Pi is off: the two STM32s are a majority and elect a leader. */
static void test_two_nodes_elect_without_the_third(void)
{
	init_all();
	s.active[NODE_N2] = true;
	s.active[NODE_N3] = true;
	/* Alone, a node has no majority and doesn't start an election. */
	for (uint32_t t = 0; t < 500; t += 5) {
		step(t);
	}
	assert(s.nodes[NODE_N2].term == 0 && s.nodes[NODE_N3].term == 0);
	assert(s.nodes[NODE_N2].wait_cluster && s.nodes[NODE_N3].wait_cluster);

	hears(NODE_N2, NODE_N3, 500);
	hears(NODE_N3, NODE_N2, 505);
	int leader = -1;

	for (uint32_t t = 500; t < 2000 && leader < 0; t++) {
		step(t);
		leader = leader_of();
	}
	assert(leader == NODE_N2 || leader == NODE_N3);
	uint32_t term = s.nodes[leader].term;

	/* Keep REF flowing so the follower stays put; then the Pi starts. */
	for (uint32_t t = 2000; t < 2200; t += 5) {
		step(t);
	}
	s.active[NODE_N1] = true;
	hears(NODE_N1, NODE_N2, 2200);
	hears(NODE_N1, NODE_N3, 2200);
	for (uint32_t t = 2200; t < 3000; t += 5) {
		step(t);
	}
	assert(s.nodes[leader].role == RAFT_LEADER && s.nodes[leader].term == term);
	assert(s.nodes[NODE_N1].role == RAFT_FOLLOWER && s.nodes[NODE_N1].leader == leader &&
	       s.nodes[NODE_N1].term == term);
	printf("random mode, Pi absent: N%d leads term %u; the Pi joins as a follower\n",
	       leader + 1, (unsigned)term);
}

/* 2. All three come up together (cockpit started, then both STM32s reset). */
static void test_seed_decides_when_all_start_together(void)
{
	init_all();
	for (int n = 0; n < NUM_NODES; n++) {
		s.active[n] = true;
	}
	for (int n = 0; n < NUM_NODES; n++) {
		for (int peer = 0; peer < NUM_NODES; peer++) {
			if (peer != n) {
				hears(n, peer, 1000 + (uint32_t)(n * 7)); /* up to ~20 ms skew */
			}
		}
	}
	int leader = -1;

	for (uint32_t t = 1000; t < 2000 && leader < 0; t++) {
		step(t);
		leader = leader_of();
	}
	assert(leader >= 0);
	printf("random mode, seed %u, all together: N%d leads term %u\n",
	       (unsigned)RAFT_ELECTION_SEED, leader + 1, (unsigned)s.nodes[leader].term);
	if (RAFT_ELECTION_SEED == RAFT_SEED_PI_WINS) {
		assert(leader == NODE_N1 && s.nodes[leader].term == 1);
	} else if (RAFT_ELECTION_SEED == RAFT_SEED_N2_WINS) {
		assert(leader == NODE_N2 && s.nodes[leader].term == 1);
	} else if (RAFT_ELECTION_SEED == RAFT_SEED_N3_WINS) {
		assert(leader == NODE_N3 && s.nodes[leader].term == 1);
	}
}

int main(void)
{
	_Static_assert(RAFT_DEFAULT_LEADER == -1, "build with -DRAFT_DEFAULT_LEADER=-1");
	test_two_nodes_elect_without_the_third();
	test_seed_decides_when_all_start_together();
	return 0;
}
