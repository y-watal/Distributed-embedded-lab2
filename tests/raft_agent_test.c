#include <assert.h>
#include <stdio.h>
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

static unsigned total_sent; /* every message any simulated node sent */

static bool send_message(void *ctx, uint32_t id, const uint8_t *body, uint8_t len)
{
	struct simulation *s = ctx;

	total_sent++;

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

/* The default leader listens RAFT_BOOT_LISTEN_MS before taking term 1. Most
 * tests start from a fresh cluster, so finish that boot right away. */
static void boot_as_default_leader(struct raft_agent *a)
{
	if (a->boot_listen) {
		a->boot_listen_until_ms = 0;
		a->boot_listen = true;
		raft_agent_poll(a, 0);
	}
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
	boot_as_default_leader(&follower);
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
	boot_as_default_leader(&leader);
	raft_agent_init(&client, NODE_N2, sink_send, &ck, 0, 2);
	boot_as_default_leader(&client);
	leader.last_index = RAFT_LOG_CAPACITY; /* log_size reached */

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
	boot_as_default_leader(&a);

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
	boot_as_default_leader(&leader);
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
	boot_as_default_leader(&client);
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

/* A COMMITTED reply for an entry this node has not applied yet must not make
 * the client re-report its own link every poll (the hop's relay flood). */
static void test_committed_reply_holds_graph_repair(void)
{
	struct sink sink = {0};
	struct raft_agent client;
	struct raft_event down = {RAFT_EVENT_CANLINK_DOWN, NODE_N3, 0, 0};

	raft_agent_init(&client, NODE_N3, sink_send, &sink, 0, 3);
	boot_as_default_leader(&client);
	client.network.link_known[NODE_N3][0] = true;
	client.network.link_up[NODE_N3][0] = true; /* stale: entry 20 not applied */
	client.applied_index = 19;
	client.commit_index = 19;
	raft_client_observe(&client, down, 1);
	raft_agent_poll(&client, 1 + RAFT_REPORT_DOWN_HOLD_MS);
	assert(client.active_request && client.active_event.type == RAFT_EVENT_CANLINK_DOWN);

	struct raft_client_reply reply = {
		.target = NODE_N3, .request_id = client.active_request_id,
		.status = RAFT_CLIENT_COMMITTED, .leader = NODE_N1, .committed_index = 20,
	};
	uint8_t body[RAFT_CLIENT_REPLY_LEN];

	raft_client_reply_encode(&reply, body);
	raft_agent_receive(&client, ID_CLIENT_REPLY(NODE_N1), body, sizeof(body), 100);
	assert(!client.active_request);

	unsigned before = sink.updates;
	for (uint32_t t = 105; t < 100 + RAFT_CLIENT_REPAIR_HOLD_MS; t += 5) {
		raft_agent_poll(&client, t);
	}
	assert(sink.updates == before && !client.active_request);

	/* Still not applied after the hold: report once more. */
	raft_agent_poll(&client, 100 + RAFT_CLIENT_REPAIR_HOLD_MS);
	assert(client.active_request && sink.updates == before + 1);

	/* Applied but a later entry disagrees again: report without waiting. */
	client.active_request = false;
	client.applied_index = client.commit_index = 21;
	raft_agent_poll(&client, 101 + RAFT_CLIENT_REPAIR_HOLD_MS);
	assert(client.active_request);
}

static void pump_rounds(struct simulation *s, uint32_t now, unsigned max_rounds);

static void pump(struct simulation *s, uint32_t now)
{
	pump_rounds(s, now, 100);
}

/* Deliver queued messages (and poll every node after each) until none remain. */
static void pump_rounds(struct simulation *s, uint32_t now, unsigned max_rounds)
{
	for (unsigned rounds = 0; rounds < max_rounds && s->count; rounds++) {
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
		boot_as_default_leader(&s.nodes[node]);
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
	boot_as_default_leader(&s.nodes[NODE_N3]);
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
		boot_as_default_leader(&s.nodes[node]);
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


/* ---- Lab 5 rest: boot listen, compaction, cockpit error, forget ---- */

struct apply_record {
	unsigned n, live, snapshots;
	uint32_t snap_index, last_index;
	bool all_replayed;
};

static void record_apply(void *ctx, uint32_t index, const struct raft_log_entry *e,
			 bool replayed, const struct raft_network *net)
{
	struct apply_record *r = ctx;

	(void)net;
	if (!e) {
		r->snapshots++;
		r->snap_index = index;
		return;
	}
	assert(index == r->last_index + 1 || r->last_index == 0 || index == r->snap_index + 1);
	r->last_index = index;
	r->n++;
	if (!replayed) {
		r->live++;
	}
}

static void test_boot_listen(void)
{
	struct sink k = {0};
	struct raft_agent pi;
	uint8_t ref[RAFT_REF_LEN] = {0};

	/* Fresh cluster: nobody else leads, so the cockpit takes term 1. */
	raft_agent_init(&pi, NODE_N1, sink_send, &k, 0, 1);
	assert(pi.boot_listen && pi.role == RAFT_FOLLOWER && pi.term == 0);
	raft_agent_poll(&pi, RAFT_BOOT_LISTEN_MS - 1);
	assert(pi.role == RAFT_FOLLOWER);
	raft_agent_poll(&pi, RAFT_BOOT_LISTEN_MS);
	assert(pi.role == RAFT_LEADER && pi.term == 1 && pi.leader == NODE_N1 && pi.boot_led);

	/* Returning cockpit: N2 leads term 3. Its REF holds off term 1 and its
	 * APPEND makes the Pi a follower. */
	raft_agent_init(&pi, NODE_N1, sink_send, &k, 1000, 1);
	raft_agent_receive(&pi, ID_REF(NODE_N2), ref, sizeof(ref), 1200);
	raft_agent_poll(&pi, 1000 + RAFT_BOOT_LISTEN_MS);
	assert(pi.role == RAFT_FOLLOWER && pi.boot_listen);

	struct raft_append ap = {
		.target = RAFT_TARGET_ALL, .term = 3, .prev_index = 5, .prev_term = 2,
		.leader_commit = 5,
	};
	uint8_t body[RAFT_APPEND_ENTRY_LEN];
	uint8_t len = raft_append_encode(&ap, body);

	raft_agent_receive(&pi, ID_APPEND(NODE_N2), body, len, 1250);
	raft_agent_poll(&pi, 1260);
	assert(pi.role == RAFT_FOLLOWER && pi.term == 3 && pi.leader == NODE_N2);
	assert(!pi.boot_listen && !pi.boot_led);
}

static void test_compaction_and_snapshot_catch_up(void)
{
	static struct simulation s;
	struct apply_record rec = {.all_replayed = true};
	const uint32_t total = RAFT_LOG_WINDOW + 40;
	uint32_t now = 10;

	memset(&s, 0, sizeof(s));
	for (int node = 0; node < NUM_NODES; node++) {
		raft_agent_init(&s.nodes[node], (uint8_t)node, send_message, &s, 0,
				(uint32_t)(node + 1));
		boot_as_default_leader(&s.nodes[node]);
	}
	s.active[NODE_N1] = true;
	s.active[NODE_N2] = true;
	s.nodes[NODE_N3].on_apply = record_apply;
	s.nodes[NODE_N3].apply_ctx = &rec;

	/* N1 and N2 commit more entries than fit in RAM while N3 is away. */
	for (uint32_t i = 0; i < total; i++) {
		struct raft_event e = {i % 2 ? RAFT_EVENT_CANLINK_UP : RAFT_EVENT_CANLINK_DOWN,
				       NODE_N2, 0, 0};

		deliver_update(&s.nodes[NODE_N1], NODE_N2, i + 1, e, now);
		raft_agent_poll(&s.nodes[NODE_N1], now);
		pump(&s, now);
		assert(s.nodes[NODE_N1].commit_index == i + 1);
		now += 5;
	}
	assert(s.nodes[NODE_N1].log_base > 0);
	assert(s.nodes[NODE_N1].last_index - s.nodes[NODE_N1].log_base <= RAFT_LOG_WINDOW);
	assert(s.nodes[NODE_N2].applied_hash == s.nodes[NODE_N1].applied_hash);

	/* N3 returns with an empty log: snapshot, then the entries still in RAM. */
	s.active[NODE_N3] = true;
	for (int k = 0; k < 30 && s.nodes[NODE_N3].applied_index < total; k++) {
		now += 100;
		raft_agent_poll(&s.nodes[NODE_N1], now);
		pump_rounds(&s, now, 2000); /* one APPEND + ACK per entry */
	}
	assert(s.nodes[NODE_N3].applied_index == total);
	assert(s.nodes[NODE_N3].applied_hash == s.nodes[NODE_N1].applied_hash);
	assert(raft_network_pack(&s.nodes[NODE_N3].network) ==
	       raft_network_pack(&s.nodes[NODE_N1].network));
	assert(rec.snapshots == 1 && rec.n == total - rec.snap_index && rec.live == 0);

	/* The next entry is live, not replayed. */
	deliver_update(&s.nodes[NODE_N1], NODE_N2, total + 1,
		       (struct raft_event){RAFT_EVENT_CANLINK_DOWN, NODE_N3, 1, 0}, now);
	raft_agent_poll(&s.nodes[NODE_N1], now);
	pump(&s, now);
	assert(s.nodes[NODE_N3].applied_index == total + 1 && rec.live == 1);
}

static void test_cockpit_error_needs_lost_n1(void)
{
	struct sink lk = {0};
	struct raft_agent leader;
	struct raft_event err = {RAFT_EVENT_COCKPIT_ERROR, NODE_N1, RAFT_BUS_NONE, 0};

	raft_agent_init(&leader, NODE_N2, sink_send, &lk, 0, 2);
	leader.role = RAFT_LEADER;
	leader.leader = NODE_N2;
	leader.term = 2;
	raft_client_observe(&leader, (struct raft_event){RAFT_EVENT_NODE_UP, NODE_N1,
						       RAFT_BUS_NONE, 0}, 0);
	deliver_update(&leader, NODE_N3, 1, err, 5);
	assert(lk.last_reply.status == RAFT_CLIENT_REJECTED && leader.last_index == 0);
	raft_client_observe(&leader, (struct raft_event){RAFT_EVENT_NODE_DOWN, NODE_N1,
						       RAFT_BUS_NONE, 0}, 10);
	deliver_update(&leader, NODE_N3, 2, err, 20);
	assert(leader.last_index == 1 && leader.pending_index == 1);
}

static void test_forget_drops_report(void)
{
	struct sink k = {0};
	struct raft_agent client;
	struct raft_event err = {RAFT_EVENT_COCKPIT_ERROR, NODE_N1, RAFT_BUS_NONE, 0};

	raft_agent_init(&client, NODE_N3, sink_send, &k, 0, 3);
	raft_client_observe(&client, err, 1);
	raft_agent_poll(&client, 1 + RAFT_REPORT_DOWN_HOLD_MS);
	assert(client.active_request && client.active_event.type == RAFT_EVENT_COCKPIT_ERROR);
	raft_client_forget(&client, err);
	assert(!client.active_request && !client.obs[0].seen);
	raft_agent_poll(&client, 500);
	assert(!client.active_request && client.queue_count == 0);
}


/* Checkoff 10: isolating the leader changes the term exactly once; when it
 * returns it follows the new leader without another election. */
static void test_isolated_leader_term_changes_once(void)
{
	static struct simulation s;

	memset(&s, 0, sizeof(s));
	for (int node = 0; node < NUM_NODES; node++) {
		raft_agent_init(&s.nodes[node], (uint8_t)node, send_message, &s, 0,
				(uint32_t)(node + 11));
		boot_as_default_leader(&s.nodes[node]);
		s.active[node] = true;
	}
	for (uint32_t t = 5; t <= 200; t += 5) { /* REFs start every follower's timer */
		for (int n = 0; n < NUM_NODES; n++) {
			raft_agent_poll(&s.nodes[n], t);
		}
		pump(&s, t);
	}
	s.active[NODE_N1] = false; /* the leader is cut off from both buses */
	for (uint32_t t = 205; t <= 2000; t += 5) {
		for (int n = 0; n < NUM_NODES; n++) {
			raft_agent_poll(&s.nodes[n], t);
		}
		pump(&s, t);
	}
	int leader = s.nodes[NODE_N2].role == RAFT_LEADER ? NODE_N2 : NODE_N3;

	assert(s.nodes[leader].role == RAFT_LEADER && s.nodes[leader].term == 2);
	assert(s.nodes[NODE_N2].term == 2 && s.nodes[NODE_N3].term == 2);
	assert(s.nodes[NODE_N1].term == 1); /* isolated: no pre-vote majority, no new term */
	s.active[NODE_N1] = true;
	for (uint32_t t = 2005; t <= 3000; t += 5) {
		for (int n = 0; n < NUM_NODES; n++) {
			raft_agent_poll(&s.nodes[n], t);
		}
		pump(&s, t);
	}
	for (int n = 0; n < NUM_NODES; n++) {
		assert(s.nodes[n].term == 2 && s.nodes[n].leader == leader);
	}
	assert(s.nodes[NODE_N1].role == RAFT_FOLLOWER);
}


/* The hop capture: the Pi kept a committed log from before a reflash while
 * N3 leads a new history. The Pi must resync, and catch-up must not storm. */
static void test_committed_conflict_resyncs_without_storm(void)
{
	static struct simulation s;
	const uint32_t n_leader = 40, n_old = 33, same = 30;

	memset(&s, 0, sizeof(s));
	for (int node = 0; node < NUM_NODES; node++) {
		raft_agent_init(&s.nodes[node], (uint8_t)node, send_message, &s, 0,
				(uint32_t)(node + 21));
		boot_as_default_leader(&s.nodes[node]);
		struct raft_agent *a = &s.nodes[node];

		a->term = 8;
		a->leader = NODE_N3;
		a->voted_for = NODE_N3;
		a->role = node == NODE_N3 ? RAFT_LEADER : RAFT_FOLLOWER;
		uint32_t n = node == NODE_N1 ? n_old : n_leader;

		a->applied_hash = RAFT_HASH_INIT;
		for (uint32_t i = 1; i <= n; i++) {
			struct raft_log_entry *e = &a->log[i - 1];

			/* N1's #31-33 come from the lost cluster: committed, older term. */
			e->term = node == NODE_N1 && i > same ? 2 : 3;
			e->event = (struct raft_event){i % 2 ? RAFT_EVENT_CANLINK_UP :
						       RAFT_EVENT_CANLINK_DOWN, NODE_N2, 0, 0};
			a->applied_hash = raft_hash_entry(a->applied_hash, e);
		}
		a->last_index = a->commit_index = a->applied_index = n;
		a->election_timer_active = false;
		s.active[node] = true;
	}
	unsigned sent_before = total_sent;

	for (uint32_t t = 1000; t <= 2000; t += 2) {
		for (int n = 0; n < NUM_NODES; n++) {
			raft_agent_poll(&s.nodes[n], t);
		}
		pump_rounds(&s, t, 2000);
	}
	unsigned sent = total_sent - sent_before;
	assert(s.nodes[NODE_N1].last_index == n_leader);
	assert(s.nodes[NODE_N1].applied_hash == s.nodes[NODE_N3].applied_hash);
	assert(s.nodes[NODE_N1].term == 8 && s.nodes[NODE_N3].role == RAFT_LEADER);
	/* One catch-up chain: ~40 entries x (APPEND + ACK) plus REF and empty
	 * APPENDs. The storm sent hundreds of messages a second. */
	printf("resync after committed conflict: %u messages in 1 s\n", sent);
	assert(sent < 400);
}

int main(void)
{
	test_commit_stops_at_matching_prefix();
	test_reject_backoff();
	test_debounce_and_coalesce();
	test_stale_peer_loss_is_rejected();
	test_recovery_cancels_stale_loss_and_repairs_graph();
	test_committed_reply_holds_graph_repair();
	test_returning_follower_catches_up();
	test_isolated_follower_does_not_raise_term();
	test_boot_listen();
	test_compaction_and_snapshot_catch_up();
	test_cockpit_error_needs_lost_n1();
	test_forget_drops_report();
	test_isolated_leader_term_changes_once();
	test_committed_conflict_resyncs_without_storm();

	struct simulation s = {0};
	struct raft_agent early_follower;

	raft_agent_init(&early_follower, NODE_N2, send_message, &s, 0, 77);
	boot_as_default_leader(&early_follower);
	raft_agent_poll(&early_follower, 1000);
	assert(early_follower.term == 1 && early_follower.role == RAFT_FOLLOWER);
	assert(!early_follower.election_timer_active);

	for (int node = 0; node < NUM_NODES; node++) {
		s.active[node] = true;
		raft_agent_init(&s.nodes[node], (uint8_t)node, send_message, &s,
				0, (uint32_t)(node + 1));
		boot_as_default_leader(&s.nodes[node]);
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
		boot_as_default_leader(&f.nodes[node]);
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
