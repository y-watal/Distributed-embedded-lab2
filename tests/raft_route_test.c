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

int main(void)
{
	struct raft_network n = {0};
	uint32_t bits = raft_route_snapshot(&n);

	/* Before the first committed observations, preserve Lab 3 delivery. */
	assert(raft_route_send_on_bus(bits, NODE_N1, 0, true));
	assert(raft_route_send_on_bus(bits, NODE_N1, 1, true));
	assert(raft_route_relay(bits, NODE_N1, 0));

	set_node(&n, NODE_N1, true, true);
	set_node(&n, NODE_N2, true, true);
	set_node(&n, NODE_N3, true, true);
	bits = raft_route_snapshot(&n);
	assert(raft_route_send_on_bus(bits, NODE_N1, 0, true));
	assert(raft_route_send_on_bus(bits, NODE_N1, 1, true));
	assert(!raft_route_relay(bits, NODE_N1, 0));
	uint32_t heard[2][NUM_NODES] = {
		{90, 90, 90}, {90, 90, 90}
	};
	assert(!raft_route_live_hop(100, heard, NODE_N1, 0, 44));
	/* Before the graph changes, direct heartbeat loss exposes the hop. */
	heard[0][NODE_N3] = 40;
	heard[1][NODE_N1] = 40;
	assert(raft_route_live_hop(100, heard, NODE_N1, 0, 44));
	assert(raft_route_live_hop(100, heard, NODE_N3, 1, 44));

	/*
	 * One missing link: once N1 is committed off A, N2 bridges in both
	 * directions before a second failure, so either pull order finds the
	 * hop already running (bench: each order alone left a ~60 ms gap that
	 * tripped the 66 ms peer-lost check). Earlier these were not relayed.
	 */
	n.link_up[NODE_N1][0] = false;
	bits = raft_route_snapshot(&n);
	assert(raft_route_relay(bits, NODE_N1, 1));  /* N1 off A: its B frames -> A */
	assert(!raft_route_relay(bits, NODE_N1, 0)); /* no A copies to send on */
	assert(raft_route_relay(bits, NODE_N3, 0));  /* N1 hears N3 only on B */
	assert(!raft_route_relay(bits, NODE_N3, 1)); /* nobody needs N3's B on A */

	/* Checkoff orientation: N1 committed off B, N3 still on both buses. */
	n.link_up[NODE_N1][0] = true;
	n.link_up[NODE_N1][1] = false;
	bits = raft_route_snapshot(&n);
	assert(raft_route_relay(bits, NODE_N1, 0));  /* covers N3 losing A next */
	assert(raft_route_relay(bits, NODE_N3, 1));  /* covers N3 losing A next, for N1 */
	assert(!raft_route_relay(bits, NODE_N3, 0));
	/* Nobody but N2 left on B: no point bridging N1 there. */
	n.link_up[NODE_N3][1] = false;
	bits = raft_route_snapshot(&n);
	assert(!raft_route_relay(bits, NODE_N1, 0));

	/* Reverse order: N3 committed off A first, N1 on both buses. Bridging
	 * N1's A traffic to B now covers N1 losing B next. */
	n.link_up[NODE_N1][1] = true;
	n.link_up[NODE_N3][0] = false;
	n.link_up[NODE_N3][1] = true;
	bits = raft_route_snapshot(&n);
	assert(raft_route_relay(bits, NODE_N1, 0));  /* N3 hears N1 only on B */
	assert(raft_route_relay(bits, NODE_N3, 1));  /* N3 is off A: its B -> A */
	assert(!raft_route_relay(bits, NODE_N1, 1));
	n.link_up[NODE_N3][0] = true;

	/* All links up again: nothing is relayed. */
	bits = raft_route_snapshot(&n);
	assert(!raft_route_relay(bits, NODE_N1, 0) && !raft_route_relay(bits, NODE_N1, 1));
	assert(!raft_route_relay(bits, NODE_N3, 0) && !raft_route_relay(bits, NODE_N3, 1));

	/* N1-A -> N2 relay -> N3-B is the two-link hop. */
	n.link_up[NODE_N1][1] = false;
	n.link_up[NODE_N3][0] = false;
	bits = raft_route_snapshot(&n);
	assert(raft_route_send_on_bus(bits, NODE_N1, 0, true));
	assert(!raft_route_send_on_bus(bits, NODE_N1, 1, true));
	assert(raft_route_send_on_bus(bits, NODE_N1, 1, false));
	assert(raft_route_relay(bits, NODE_N1, 0));
	assert(raft_route_relay(bits, NODE_N3, 1));

	/* Mirrored hop: N1-B -> N2 -> N3-A. */
	n.link_up[NODE_N1][0] = false;
	n.link_up[NODE_N1][1] = true;
	n.link_up[NODE_N3][0] = true;
	n.link_up[NODE_N3][1] = false;
	bits = raft_route_snapshot(&n);
	assert(raft_route_relay(bits, NODE_N1, 1));
	assert(raft_route_relay(bits, NODE_N3, 0));

	/* When B reaches nobody, data traffic uses A. */
	set_node(&n, NODE_N1, true, true);
	set_node(&n, NODE_N2, true, false);
	set_node(&n, NODE_N3, true, false);
	bits = raft_route_snapshot(&n);
	assert(raft_route_send_on_bus(bits, NODE_N1, 0, true));
	assert(!raft_route_send_on_bus(bits, NODE_N1, 1, true));
	assert(!raft_route_relay(bits, NODE_N1, 0));

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
	return 0;
}
