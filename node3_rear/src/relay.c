#include "relay.h"

#include <zephyr/kernel.h>

#include "canbus.h"
#include "hb.h"
#include "lab3_config.h"
#include "node_config.h"
#include "raft_node.h"
#include "raft_route.h"
#include "tp.h"
#include "wcet.h"

struct relay_stats relay_stats;

#if RELAY_ENABLED

#define RELAY_STACK_SIZE  1024
#define RELAY_QUEUE_DEPTH 16
#define RELAY_URGENT_DEPTH 8

struct relay_item {
	uint8_t src_bus;
	uint32_t t_rx; /* rx_cb time: J's start */
	struct can_frame frame;
};
K_MSGQ_DEFINE(relay_q, sizeof(struct relay_item), RELAY_QUEUE_DEPTH, 4);
K_MSGQ_DEFINE(relay_urgent_q, sizeof(struct relay_item), RELAY_URGENT_DEPTH, 4);
K_SEM_DEFINE(relay_due, 0, RELAY_QUEUE_DEPTH + RELAY_URGENT_DEPTH);

K_THREAD_STACK_DEFINE(relay_stack, RELAY_STACK_SIZE);
static struct k_thread relay_data;

/* CTRL and FAULT: relayed ahead of everything else. */
static bool is_urgent(const struct can_frame *frame)
{
	return frame->id == ID_CTRL ||
	       (frame->id >= ID_FAULT(0) && frame->id < ID_FAULT(NUM_NODES));
}

/* Dispatch thread, first copy only: hand it to RELAY without waiting. */
static void relay_hook(const struct can_frame *frame, int bus)
{
	struct relay_item item = {
		.src_bus = (uint8_t)bus, .t_rx = canbus_rx_cycles(), .frame = *frame};

	struct k_msgq *queue = is_urgent(frame) ? &relay_urgent_q : &relay_q;

	if (k_msgq_put(queue, &item, K_NO_WAIT) != 0) {
		atomic_inc(&relay_stats.dropped);
	} else {
		k_sem_give(&relay_due);
	}
}

static bool should_relay(const struct relay_item *item)
{
	int sender = catalog_sender(item->frame.id);

	if (raft_route_relay(raft_node_route_snapshot(), sender, item->src_bus)) {
		return true;
	}
	/* A newly broken link can precede its committed graph entry. N2's direct
	 * heartbeat observations provide a temporary route during that gap. */
	uint32_t last_hb[2][NUM_NODES] = {{0}};

	for (int bus = 0; bus < 2; bus++) {
		last_hb[bus][NODE_N1] = canbus_last_hb_ms(bus, NODE_N1);
		last_hb[bus][NODE_N3] = canbus_last_hb_ms(bus, NODE_N3);
	}
	return raft_route_live_hop(k_uptime_get_32(), last_hb, sender,
		item->src_bus, DEG_TIMEOUT_MS);
}

static void forward(const struct relay_item *item);

/* Forward urgent frames that arrived while a lower-priority frame waits. */
static void drain_urgent(void)
{
	struct relay_item urgent;

	while (k_msgq_get(&relay_urgent_q, &urgent, K_NO_WAIT) == 0) {
		(void)k_sem_take(&relay_due, K_NO_WAIT); /* its count */
		forward(&urgent);
	}
}

/*
 * Retry while every MCP2515 TX buffer is busy instead of dropping the frame
 * (RELAY_RAFT_WAIT_US / RELAY_LAB3_WAIT_US). Non-urgent frames let queued
 * CTRL and FAULT go first between retries; urgent ones never drain, so this
 * cannot recurse more than one level.
 */
static int send_relay(int dst, const struct relay_item *item, bool let_urgent_pass)
{
	uint32_t limit = catalog_is_raft(item->frame.id) ? RELAY_RAFT_WAIT_US
							 : RELAY_LAB3_WAIT_US;
	uint32_t start = k_cycle_get_32();

	for (;;) {
		int ret = canbus_forward_try(dst, &item->frame, item->t_rx);

		if (ret != -EAGAIN) {
			return ret;
		}
		if (k_cyc_to_us_floor32(k_cycle_get_32() - start) >= limit) {
			canbus_note_tx_full(dst);
			atomic_inc(&relay_stats.full);
			return ret;
		}
		if (let_urgent_pass) {
			drain_urgent();
		}
		k_sleep(K_USEC(100));
	}
}

static void forward(const struct relay_item *item)
{
	int dst = item->src_bus == BUS_A ? BUS_B : BUS_A;

	if (!should_relay(item)) {
		atomic_inc(&relay_stats.skipped);
		return;
	}
	if (canbus_is_degraded(dst) || !canbus_bus_running(dst)) {
		atomic_inc(&relay_stats.skipped);
		return;
	}
	tp_toggle(TP_RELAY_TX);
	if (send_relay(dst, item, !is_urgent(&item->frame)) == 0) {
		atomic_inc(&relay_stats.forwarded);
	}
}

/*
 * A bulk Raft fragment prefers not to take the last MCP2515 TX buffer. Wait
 * for room in short sleeps, forwarding any CTRL or FAULT that arrives
 * meanwhile, so urgent frames never queue behind Raft. Returns false if the
 * wait ran out; the fragment is then relayed anyway.
 */
static bool wait_raft_room(int dst)
{
	uint32_t start = k_cycle_get_32();
	while (!canbus_raft_tx_ready(dst)) {
		drain_urgent();
		if (k_cyc_to_us_floor32(k_cycle_get_32() - start) >= RAFT_TX_RESERVE_WAIT_US) {
			return false;
		}
		k_sleep(K_USEC(100));
	}
	return true;
}

static void relay_thread(void *p1, void *p2, void *p3)
{
	struct relay_item item;
	struct wcet_job job = WCET_JOB(WCET_RELAY);

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		wcet_end(&job);
		k_sem_take(&relay_due, K_FOREVER);
		if (k_msgq_get(&relay_urgent_q, &item, K_NO_WAIT) != 0) {
			if (k_msgq_get(&relay_q, &item, K_NO_WAIT) != 0) {
				continue;
			}
		}
		wcet_begin(&job);

		int dst = item.src_bus == BUS_A ? BUS_B : BUS_A;
		bool raft_bulk = catalog_is_raft(item.frame.id) &&
				 (item.frame.id & ~3u) != ID_REF(0);

		if (raft_bulk && canbus_bus_running(dst) && !canbus_is_degraded(dst) &&
		    should_relay(&item) &&
		    !wait_raft_room(dst)) {
			atomic_inc(&relay_stats.raft_late); /* relayed without the reserve */
		}
		forward(&item);
	}
}

int relay_init(void)
{
	k_thread_create(&relay_data, relay_stack, K_THREAD_STACK_SIZEOF(relay_stack),
			relay_thread, NULL, NULL, NULL, PRIO_RELAY, 0, K_NO_WAIT);
	k_thread_name_set(&relay_data, "relay");
	canbus_set_relay_hook(relay_hook);
	hb_set_relay_active(true);
	return 0;
}

#else

int relay_init(void)
{
	return 0;
}

#endif /* RELAY_ENABLED */
