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

	/* One missing link needs no bridge: N3 sends directly on both buses.
	 * This was previously relayed onto B because N1 was missing A. */
	n.link_up[NODE_N1][0] = false;
	bits = raft_route_snapshot(&n);
	assert(!raft_route_relay(bits, NODE_N3, 0));
	assert(!raft_route_relay(bits, NODE_N3, 1));
	/* N1 is committed off A, so its B frames are bridged onto A before N3
	 * needs them. Without this, pulling N3's B next left N3 deaf to N1 for
	 * ~60 ms on the bench (N3 logged "N1 lost"). */
	assert(raft_route_relay(bits, NODE_N1, 1));
	assert(!raft_route_relay(bits, NODE_N1, 0)); /* no A copies to send on */

	/* Checkoff orientation: N1 committed off B, N3 still on both buses. */
	n.link_up[NODE_N1][0] = true;
	n.link_up[NODE_N1][1] = false;
	bits = raft_route_snapshot(&n);
	assert(raft_route_relay(bits, NODE_N1, 0));  /* pre-emptive A -> B */
	assert(!raft_route_relay(bits, NODE_N3, 0)); /* N3 reaches B itself */
	assert(!raft_route_relay(bits, NODE_N3, 1));
	/* Nobody but N2 left on B: no point bridging N1 there. */
	n.link_up[NODE_N3][1] = false;
	bits = raft_route_snapshot(&n);
	assert(!raft_route_relay(bits, NODE_N1, 0));
	n.link_up[NODE_N3][1] = true;
	n.link_up[NODE_N1][1] = true;
	n.link_up[NODE_N1][0] = false;
	bits = raft_route_snapshot(&n);
	n.link_up[NODE_N1][0] = true;
	n.link_up[NODE_N3][1] = false;
	bits = raft_route_snapshot(&n);
	assert(!raft_route_relay(bits, NODE_N1, 0));
	assert(!raft_route_relay(bits, NODE_N1, 1));
	n.link_up[NODE_N3][1] = true;

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
