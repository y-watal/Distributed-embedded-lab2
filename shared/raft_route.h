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
	/* The graph decides only whether anyone is reachable on this bus. Our
	 * own link is a local fact the caller already checked (bus running, not
	 * degraded): trusting the graph for it deadlocked N3 on the bench, whose
	 * stale graph marked its own links down, so it stopped sending the very
	 * acks that would have brought the graph up to date. */
	return raft_route_has_peer(bits, self, bus);
}

/*
 * The one node that bridges the buses for frames from sender: the lowest-ID
 * live node, other than the sender, with both links. Every node computes the
 * same answer from the same committed graph, so exactly one node relays a
 * given sender's traffic and no node is special. -1 if none can.
 */
static inline int raft_route_bridge(uint32_t bits, int sender)
{
	for (int node = 0; node < NUM_NODES; node++) {
		if (node != sender && raft_route_node_may_be_up(bits, node) &&
		    raft_route_link_may_be_up(bits, node, 0) &&
		    raft_route_link_may_be_up(bits, node, 1)) return node;
	}
	return -1;
}

static inline bool raft_route_link_known_down(uint32_t bits, int node, int bus)
{
	int link = node * 2 + bus;

	return (bits & (1u << (6 + link))) && !(bits & (1u << (12 + link)));
}

/*
 * Graph-only relay decision, identical on every node. self forwards a frame
 * from sender, received on src_bus, onto the other bus if and only if:
 *   - self is the bridge for that sender (raft_route_bridge);
 *   - the sender is not known to be on the other bus itself; and
 *   - some live node other than the sender and self is committed off
 *     src_bus but may be on the other bus, so it cannot hear the frame
 *     any other way.
 * Until a link change is committed nothing is relayed for it; the handout
 * (Part 5.2) expects a few rounds before the routes are set up.
 */
static inline bool raft_route_relay(uint32_t bits, int self, int sender, int src_bus)
{
	int dst_bus = 1 - src_bus;

	if (sender < 0 || sender >= NUM_NODES || sender == self) return false;
	if (raft_route_bridge(bits, sender) != self) return false;
	int sender_dst_link = sender * 2 + dst_bus;
	if ((bits & (1u << (6 + sender_dst_link))) &&
	    (bits & (1u << (12 + sender_dst_link)))) return false;
	for (int node = 0; node < NUM_NODES; node++) {
		if (node != sender && node != self &&
		    raft_route_node_may_be_up(bits, node) &&
		    raft_route_link_known_down(bits, node, src_bus) &&
		    raft_route_link_may_be_up(bits, node, dst_bus)) return true;
	}
	return false;
}

#endif
