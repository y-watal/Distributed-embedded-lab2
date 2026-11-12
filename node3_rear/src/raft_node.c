#include "raft_node.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "canbus.h"
#include "node_config.h"
#include "raft_transport.h"

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
		k_mutex_unlock(&raft_lock);
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

void raft_node_print_status(void)
{
	uint32_t term, last_index, commit_index;
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
	k_mutex_unlock(&raft_lock);
	if (leader < 0) {
		printk("  raft: term %u leader unknown %s log %u commit %u\n", term,
		       role == RAFT_CANDIDATE ? "CANDIDATE" : "FOLLOWER",
		       last_index, commit_index);
	} else {
		printk("  raft: term %u leader N%d %s log %u commit %u\n",
		       term, leader + 1,
		       role == RAFT_LEADER ? "LEADER" : "FOLLOWER",
		       last_index, commit_index);
	}
	printk("  network:");
	for (int n = 0; n < NUM_NODES; n++) {
		printk(" N%d=%c A%c B%c", n + 1,
		       network.node_known[n] ? (network.node_up[n] ? 'U' : 'D') : '?',
		       network.link_known[n][0] ? (network.link_up[n][0] ? 'U' : 'D') : '?',
		       network.link_known[n][1] ? (network.link_up[n][1] ? 'U' : 'D') : '?');
	}
	printk(" cockpit=%c\n", network.cockpit_known ?
	       (network.cockpit_ok ? 'U' : 'D') : '?');
}
