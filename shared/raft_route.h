#ifndef SHARED_RAFT_ROUTE_H
#define SHARED_RAFT_ROUTE_H

#include <stdint.h>

#include "raft_agent.h"

/* One atomic word lets the time-critical CAN paths read the committed graph
 * without waiting for the Raft worker. Unknown links remain usable until
 * there is enough committed information to choose a path. */
static inline uint32_t raft_route_snapshot(const struct raft_network *n)
{
	uint32_t bits = 0;

	for (int node = 0; node < NUM_NODES; node++) {
		if (n->node_known[node]) bits |= 1u << node;
		if (n->node_up[node]) bits |= 1u << (3 + node);
		for (int bus = 0; bus < 2; bus++) {
			int link = node * 2 + bus;
			if (n->link_known[node][bus]) bits |= 1u << (6 + link);
			if (n->link_up[node][bus]) bits |= 1u << (12 + link);
		}
	}
	return bits;
}

static inline bool raft_route_node_may_be_up(uint32_t bits, int node)
{
	return !(bits & (1u << node)) || (bits & (1u << (3 + node)));
}

static inline bool raft_route_link_may_be_up(uint32_t bits, int node, int bus)
{
	int link = node * 2 + bus;

	return !(bits & (1u << (6 + link))) || (bits & (1u << (12 + link)));
}

static inline bool raft_route_has_peer(uint32_t bits, int self, int bus)
{
	for (int node = 0; node < NUM_NODES; node++) {
		if (node != self && raft_route_node_may_be_up(bits, node) &&
		    raft_route_link_may_be_up(bits, node, bus)) return true;
	}
	return false;
}

static inline bool raft_route_send_on_bus(uint32_t bits, int self, int bus,
						 bool other_usable)
{
	/* Never trust a stale graph over the only locally usable bus. */
	if (!other_usable || !raft_route_has_peer(bits, self, 1 - bus)) return true;
	return raft_route_link_may_be_up(bits, self, bus) &&
	       raft_route_has_peer(bits, self, bus);
}

static inline bool raft_route_relay(uint32_t bits, int sender, int src_bus)
{
	int dst_bus = 1 - src_bus;

	if (sender < 0 || sender >= NUM_NODES || sender == NODE_N2) return false;
	/* When the sender is already on the destination bus, its own copy
	 * reaches any destination there. N2 need not forward another copy. */
	int sender_dst_link = sender * 2 + dst_bus;
	if ((bits & (1u << (6 + sender_dst_link))) &&
	    (bits & (1u << (12 + sender_dst_link)))) return false;
	/* The sender is committed off the destination bus: bridge its frames
	 * there before anyone needs them. If a listener on that bus loses its
	 * other link next, the hop is already in place; detecting it afterwards
	 * left a ~60 ms gap that tripped the 66 ms peer-lost check on the bench. */
	if (bits & (1u << (6 + sender_dst_link))) {
		for (int node = 0; node < NUM_NODES; node++) {
			if (node != sender && node != NODE_N2 &&
			    raft_route_node_may_be_up(bits, node) &&
			    raft_route_link_may_be_up(bits, node, dst_bus)) return true;
		}
	}
	/* An incomplete or stale graph cannot safely suppress the Lab 3 relay. */
	if (!raft_route_link_may_be_up(bits, sender, src_bus)) return true;
	for (int node = 0; node < NUM_NODES; node++) {
		if (node == sender || node == NODE_N2) continue;
		if (!(bits & (1u << node)) ||
		    !(bits & (1u << (6 + node * 2 + src_bus))) ||
		    !(bits & (1u << (6 + node * 2 + dst_bus)))) return true;
		if (raft_route_node_may_be_up(bits, node) &&
		    !raft_route_link_may_be_up(bits, node, src_bus) &&
		    raft_route_link_may_be_up(bits, node, dst_bus)) return true;
	}
	return false;
}

/* N2 sees N1/N3 heartbeats directly on each physical bus. While a link
 * change has not yet committed, those fresh observations can reveal a hop. */
static inline bool raft_route_live_hop(uint32_t now_ms,
				      const uint32_t last_hb[2][NUM_NODES],
				      int sender, int src_bus, uint32_t timeout_ms)
{
	int dst_bus = 1 - src_bus;

	if (sender < 0 || sender >= NUM_NODES || sender == NODE_N2) return false;
	uint32_t sender_dst = last_hb[dst_bus][sender];
	if (sender_dst && now_ms - sender_dst <= timeout_ms) return false;
	for (int node = 0; node < NUM_NODES; node++) {
		if (node == sender || node == NODE_N2) continue;
		uint32_t peer_dst = last_hb[dst_bus][node];
		uint32_t peer_src = last_hb[src_bus][node];
		if (peer_dst && now_ms - peer_dst <= timeout_ms &&
		    (!peer_src || now_ms - peer_src > timeout_ms)) return true;
	}
	return false;
}

#endif
