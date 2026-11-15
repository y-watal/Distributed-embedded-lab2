#include "raft_node.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <string.h>

#include "canbus.h"
#include "node_config.h"
#include "raft_transport.h"
#include "raft_route.h"
#include "raft_status.h"
#include "gtime.h"
#include "tp.h"

#define RAFT_STACK_SIZE 3072
#define RAFT_QUEUE_DEPTH 32
#define PRIO_RAFT 9 /* below CTRL, relay, and HB_TX */

struct raft_work {
	uint8_t kind; /* 0 = CAN frame, 1 = local client event */
	struct can_frame frame;
	struct raft_event event;
};
K_MSGQ_DEFINE(raft_q, sizeof(struct raft_work), RAFT_QUEUE_DEPTH, 4);
K_MUTEX_DEFINE(raft_lock);
K_THREAD_STACK_DEFINE(raft_stack, RAFT_STACK_SIZE);

static struct k_thread raft_thread_data;
static struct raft_agent agent;
static struct raft_transport transport;
static atomic_t route_snapshot;
/* bits 0-7: leader + 1 (0 = unknown); bit 8: this node leads */
static atomic_t leader_snapshot;

static bool emit_frame(void *ctx, uint32_t id, const uint8_t frame[FRAME_DLC])
{
	ARG_UNUSED(ctx);
	return canbus_send_raft(id, frame) == 0;
}

static bool send_body(void *ctx, uint32_t id, const uint8_t *body, uint8_t len)
{
	ARG_UNUSED(ctx);
	return raft_transport_send(&transport, id, body, len, emit_frame, NULL);
}

static void raft_thread(void *p1, void *p2, void *p3)
{
	struct raft_work work;
	uint8_t body[RAFT_MAX_BODY_LEN];
	uint8_t len;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	for (;;) {
		uint32_t wait_ms = raft_agent_next_wake_ms(&agent, k_uptime_get_32());
		int got = k_msgq_get(&raft_q, &work, K_MSEC(wait_ms));
		uint32_t now = k_uptime_get_32();

		k_mutex_lock(&raft_lock, K_FOREVER);
		if (got == 0 && work.kind == 0 &&
		    raft_transport_receive(&transport, work.frame.id, work.frame.data,
					   now, body, &len)) {
			raft_agent_receive(&agent, work.frame.id, body, len, now);
		} else if (got == 0 && work.kind == 1) {
			raft_client_observe(&agent, work.event, now);
		}
		raft_agent_poll(&agent, now);
		atomic_set(&route_snapshot, (atomic_val_t)raft_route_snapshot(&agent.network));
		atomic_set(&leader_snapshot, (atomic_val_t)((agent.leader + 1) |
			(agent.role == RAFT_LEADER ? 0x100 : 0)));
		bool leads = agent.role == RAFT_LEADER;
		k_mutex_unlock(&raft_lock);
		tp_set(TP_LEADER, leads);
		tp_set(TP_SYNC, gtime_is_synced());
	}
}

int raft_node_init(void)
{
	raft_agent_init(&agent, NODE_SELF, send_body, NULL, k_uptime_get_32(),
			k_cycle_get_32() ^ (0x9e3779b9u * (NODE_SELF + 1u)));
	if (agent.leader < 0) {
		printk("Raft N%d: term %u, leader unknown, role follower\n",
		       NODE_SELF + 1, agent.term);
	} else {
		printk("Raft N%d: term %u, leader N%d, role %s\n", NODE_SELF + 1,
		       agent.term, agent.leader + 1,
		       agent.role == RAFT_LEADER ? "leader" : "follower");
	}
	atomic_set(&leader_snapshot, (atomic_val_t)((agent.leader + 1) |
		(agent.role == RAFT_LEADER ? 0x100 : 0)));
	k_thread_create(&raft_thread_data, raft_stack, K_THREAD_STACK_SIZEOF(raft_stack),
			raft_thread, NULL, NULL, NULL, PRIO_RAFT, 0, K_NO_WAIT);
	k_thread_name_set(&raft_thread_data, "raft");
	raft_node_report_event((struct raft_event){RAFT_EVENT_NODE_UP, NODE_SELF,
					     RAFT_BUS_NONE, 0});
	return 0;
}

void raft_node_receive(const struct can_frame *frame)
{
	struct raft_work work = {.kind = 0, .frame = *frame};

	(void)k_msgq_put(&raft_q, &work, K_NO_WAIT);
}

bool raft_node_report_event(struct raft_event event)
{
	struct raft_work work = {.kind = 1, .event = event};

	return k_msgq_put(&raft_q, &work, K_NO_WAIT) == 0;
}

void raft_node_get_network(struct raft_network *out)
{
	k_mutex_lock(&raft_lock, K_FOREVER);
	raft_client_get_network(&agent, out);
	k_mutex_unlock(&raft_lock);
}

uint32_t raft_node_route_snapshot(void)
{
	return (uint32_t)atomic_get(&route_snapshot);
}

void raft_node_leader_snapshot(int *leader, bool *is_leader)
{
	atomic_val_t v = atomic_get(&leader_snapshot);

	*leader = (int)(v & 0xFF) - 1;
	*is_leader = (v & 0x100) != 0;
}

void raft_node_print_status(void)
{
	uint32_t term, last_index, commit_index;
	static struct raft_log_entry log_snapshot[RAFT_WORKING_LOG_ENTRIES];
	static uint32_t printed_last, printed_commit;
	static bool printed;
	bool print_log;
	int leader;
	enum raft_role role;
	struct raft_network network;

	k_mutex_lock(&raft_lock, K_FOREVER);
	term = agent.term;
	leader = agent.leader;
	role = agent.role;
	last_index = agent.last_index;
	commit_index = agent.commit_index;
	network = agent.network;
	print_log = !printed || printed_last != last_index || printed_commit != commit_index;
	if (print_log) {
		memcpy(log_snapshot, agent.log, last_index * sizeof(log_snapshot[0]));
		printed_last = last_index;
		printed_commit = commit_index;
		printed = true;
	}
	k_mutex_unlock(&raft_lock);
	uint32_t log_hash = raft_committed_log_hash(log_snapshot, commit_index);
	if (leader < 0) {
		printk("  RAFT: leader=? term=%u role=%s | committed=%u total=%u | log=%08x\n", term,
		       role == RAFT_CANDIDATE ? "CANDIDATE" : "FOLLOWER",
		       commit_index, last_index, log_hash);
	} else {
		printk("  RAFT: leader=N%d term=%u role=%s | committed=%u total=%u | log=%08x\n",
		       leader + 1, term,
		       role == RAFT_LEADER ? "LEADER" : "FOLLOWER",
		       commit_index, last_index, log_hash);
	}
	printk("  GRAPH:");
	for (int n = 0; n < NUM_NODES; n++) {
		printk(" N%d:%s(A:%s B:%s)", n + 1,
		       raft_state_text(network.node_known[n], network.node_up[n]),
		       raft_state_text(network.link_known[n][0], network.link_up[n][0]),
		       raft_state_text(network.link_known[n][1], network.link_up[n][1]));
	}
	printk(" cockpit:%s\n", !network.cockpit_known ? "?" :
	       (network.cockpit_ok ? "OK" : "ERROR"));
	if (print_log) {
		printk("  LOG SNAPSHOT: %u entries, committed through #%u\n",
		       last_index, commit_index);
		for (uint32_t i = 0; i < last_index; i++) {
			const struct raft_event *e = &log_snapshot[i].event;
			char bus = e->bus < 2 ? (char)('A' + e->bus) : '-';

			printk("    [%u] term %u %s %s N%u bus %c value %u\n",
			       i + 1, log_snapshot[i].term,
			       i < commit_index ? "COMMITTED" : "PENDING",
			       raft_event_name(e->type), (unsigned)e->node + 1,
			       bus, (unsigned)e->value);
		}
	}
}
