#include <assert.h>
#include <string.h>

#include "raft_route.h"

static void set_node(struct raft_network *n, int node, bool a, bool b)
{
	n->node_known[node] = true;
	n->node_up[node] = true;
	n->link_known[node][0] = true;
	n->link_known[node][1] = true;
	n->link_up[node][0] = a;
	n->link_up[node][1] = b;
}

static void all_up(struct raft_network *n)
{
	memset(n, 0, sizeof(*n));
	for (int node = 0; node < NUM_NODES; node++) {
		set_node(n, node, true, true);
	}
}

/* Which node, if any, relays sender's frames seen on src_bus. -1 for none;
 * asserts that at most one node does. */
static int relayer(uint32_t bits, int sender, int src_bus)
{
	int who = -1;

	for (int self = 0; self < NUM_NODES; self++) {
		if (raft_route_relay(bits, self, sender, src_bus)) {
			assert(who == -1);
			who = self;
		}
	}
	return who;
}

static void test_send_on_bus(void)
{
	struct raft_network n = {0};
	uint32_t bits = raft_route_snapshot(&n);

	/* Before the first committed observations, send on both buses. */
	assert(raft_route_send_on_bus(bits, NODE_N1, 0, true));
	assert(raft_route_send_on_bus(bits, NODE_N1, 1, true));

	all_up(&n);
	bits = raft_route_snapshot(&n);
	assert(raft_route_send_on_bus(bits, NODE_N1, 0, true));
	assert(raft_route_send_on_bus(bits, NODE_N1, 1, true));

	/* N1 off B: send on A only, unless A is unusable locally. */
	n.link_up[NODE_N1][1] = false;
	n.link_up[NODE_N3][0] = false;
	bits = raft_route_snapshot(&n);
	assert(raft_route_send_on_bus(bits, NODE_N1, 0, true));
	assert(!raft_route_send_on_bus(bits, NODE_N1, 1, true));
	assert(raft_route_send_on_bus(bits, NODE_N1, 1, false));

	/* When B reaches nobody, data traffic uses A. */
	set_node(&n, NODE_N1, true, true);
	set_node(&n, NODE_N2, true, false);
	set_node(&n, NODE_N3, true, false);
	bits = raft_route_snapshot(&n);
	assert(raft_route_send_on_bus(bits, NODE_N1, 0, true));
	assert(!raft_route_send_on_bus(bits, NODE_N1, 1, true));

	/* A stale graph with no paths must not block recovery traffic. */
	memset(&n, 0, sizeof(n));
	for (int node = 0; node < NUM_NODES; node++) {
		n.node_known[node] = true;
		n.link_known[node][0] = true;
		n.link_known[node][1] = true;
	}
	bits = raft_route_snapshot(&n);
	assert(raft_route_send_on_bus(bits, NODE_N1, 0, true));
	assert(raft_route_send_on_bus(bits, NODE_N1, 1, true));
}

static void test_relay(void)
{
	struct raft_network n = {0};
	uint32_t bits = raft_route_snapshot(&n);

	/* Empty graph: nobody is committed off a bus, so nothing is relayed. */
	for (int s = 0; s < NUM_NODES; s++) {
		assert(relayer(bits, s, 0) == -1 && relayer(bits, s, 1) == -1);
	}

	/* All links up: nothing is relayed. */
	all_up(&n);
	bits = raft_route_snapshot(&n);
	for (int s = 0; s < NUM_NODES; s++) {
		assert(relayer(bits, s, 0) == -1 && relayer(bits, s, 1) == -1);
	}
	/* The bridge is the lowest-ID live node with both links, not the sender. */
	assert(raft_route_bridge(bits, NODE_N1) == NODE_N2);
	assert(raft_route_bridge(bits, NODE_N2) == NODE_N1);
	assert(raft_route_bridge(bits, NODE_N3) == NODE_N1);

	/* One link down: N1 off B. N3 still hears N1 on A; N1 hears N3 on A. */
	n.link_up[NODE_N1][1] = false;
	bits = raft_route_snapshot(&n);
	for (int s = 0; s < NUM_NODES; s++) {
		assert(relayer(bits, s, 0) == -1 && relayer(bits, s, 1) == -1);
	}

	/* Checkoff hop: N1 off B, N3 off A. N2 bridges both directions. */
	n.link_up[NODE_N3][0] = false;
	bits = raft_route_snapshot(&n);
	assert(relayer(bits, NODE_N1, 0) == NODE_N2); /* N1 on A -> N3 on B */
	assert(relayer(bits, NODE_N3, 1) == NODE_N2); /* N3 on B -> N1 on A */
	assert(relayer(bits, NODE_N2, 0) == -1);      /* N2 sends on both */
	assert(relayer(bits, NODE_N2, 1) == -1);

	/* Mirrored hop: N1 off A, N3 off B. */
	all_up(&n);
	n.link_up[NODE_N1][0] = false;
	n.link_up[NODE_N3][1] = false;
	bits = raft_route_snapshot(&n);
	assert(relayer(bits, NODE_N1, 1) == NODE_N2);
	assert(relayer(bits, NODE_N3, 0) == NODE_N2);

	/* N2 off A and N3 off B: the Pi is the only node on both buses, so it
	 * bridges. No node is special. */
	all_up(&n);
	n.link_up[NODE_N2][0] = false;
	n.link_up[NODE_N3][1] = false;
	bits = raft_route_snapshot(&n);
	assert(relayer(bits, NODE_N2, 1) == NODE_N1); /* N2 on B -> N3 on A */
	assert(relayer(bits, NODE_N3, 0) == NODE_N1); /* N3 on A -> N2 on B */
	assert(relayer(bits, NODE_N1, 0) == -1);      /* N1 is on both buses */

	/* N1 off A and N2 off B: N3 bridges. */
	all_up(&n);
	n.link_up[NODE_N1][0] = false;
	n.link_up[NODE_N2][1] = false;
	bits = raft_route_snapshot(&n);
	assert(relayer(bits, NODE_N1, 1) == NODE_N3);
	assert(relayer(bits, NODE_N2, 0) == NODE_N3);

	/* A node that is down cannot bridge: N2 down, N3 off A. Nobody else is
	 * on both buses, so nothing can be relayed. */
	all_up(&n);
	n.node_up[NODE_N2] = false;
	n.link_up[NODE_N2][0] = n.link_up[NODE_N2][1] = false;
	n.link_up[NODE_N3][0] = false;
	bits = raft_route_snapshot(&n);
	assert(raft_route_bridge(bits, NODE_N1) == -1);
	assert(relayer(bits, NODE_N1, 0) == -1);

	/* Nobody is relayed onto a bus with no listener: N1 off B, N3 off B. */
	all_up(&n);
	n.link_up[NODE_N1][1] = false;
	n.link_up[NODE_N3][1] = false;
	bits = raft_route_snapshot(&n);
	for (int s = 0; s < NUM_NODES; s++) {
		assert(relayer(bits, s, 0) == -1);
	}
}

int main(void)
{
	test_send_on_bus();
	test_relay();
	return 0;
}
