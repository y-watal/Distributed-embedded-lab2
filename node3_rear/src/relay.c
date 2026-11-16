#include "relay.h"

#include <zephyr/kernel.h>

#include "canbus.h"
#include "hb.h"
#include "lab3_config.h"
#include "node_config.h"
#include "tp.h"
#include "wcet.h"
#include "gtime.h"

struct relay_stats relay_stats;

#if RELAY_ENABLED

#define RELAY_STACK_SIZE  1024
#define RELAY_QUEUE_DEPTH 32 /* a Raft fragment may now wait up to ~3 ms for a buffer */
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

/* Dispatch thread, first copy only: hand it to RELAY without waiting. */
static void relay_hook(const struct can_frame *frame, int bus)
{
	struct relay_item item = {
		.src_bus = (uint8_t)bus, .t_rx = canbus_rx_cycles(), .frame = *frame};

	bool urgent = frame->id == ID_CTRL ||
		      (frame->id >= ID_FAULT(0) && frame->id < ID_FAULT(NUM_NODES));
	struct k_msgq *queue = urgent ? &relay_urgent_q : &relay_q;

	if (k_msgq_put(queue, &item, K_NO_WAIT) != 0) {
		atomic_inc(&relay_stats.dropped);
	} else {
		k_sem_give(&relay_due);
	}
}

static void forward(const struct relay_item *item)
{
	int dst = item->src_bus == BUS_A ? BUS_B : BUS_A;

	if (canbus_is_degraded(dst) || !canbus_bus_running(dst)) {
		atomic_inc(&relay_stats.skipped);
		return;
	}
	struct can_frame out = item->frame;

	if ((out.id & ~3u) == ID_REF(0)) {
		/* Our own global time replaces the leader's, so the relay delay
		 * is not seen as clock offset on the far side. */
		gtime_restamp_relayed_ref(&out);
	}
	tp_toggle(TP_RELAY_TX);
	if (canbus_forward_at(dst, &out, item->t_rx) == 0) {
		atomic_inc(&relay_stats.forwarded);
	}
}

/*
 * A bulk Raft fragment prefers not to take the last MCP2515 TX buffer. Wait
 * for room in short sleeps, forwarding any CTRL or FAULT that arrives
 * meanwhile, so urgent frames never queue behind Raft. Returns false if the
 * wait ran out; the fragment is then relayed anyway.
 */
static void forward_waiting_urgent(void)
{
	struct relay_item urgent;

	while (k_msgq_get(&relay_urgent_q, &urgent, K_NO_WAIT) == 0) {
		(void)k_sem_take(&relay_due, K_NO_WAIT); /* its count */
		forward(&urgent);
	}
}

static bool wait_raft_room(int dst)
{
	uint32_t start = k_cycle_get_32();

	while (!canbus_raft_tx_ready(dst)) {
		forward_waiting_urgent();
		if (k_cyc_to_us_floor32(k_cycle_get_32() - start) >= RAFT_TX_RESERVE_WAIT_US) {
			return false;
		}
		k_sleep(K_USEC(100));
	}
	return true;
}

/*
 * Bulk Raft fragment: never dropped just because the TX buffers are briefly
 * full (the driver frees a buffer a little after its TX callback). Retry in
 * RELAY_RAFT_TRY_US slices for up to RELAY_RAFT_TX_WAIT_US, forwarding any
 * CTRL or FAULT between tries, like N2's own Raft sends.
 */
static void forward_raft(const struct relay_item *item)
{
	int dst = item->src_bus == BUS_A ? BUS_B : BUS_A;

	if (canbus_is_degraded(dst) || !canbus_bus_running(dst)) {
		atomic_inc(&relay_stats.skipped);
		return;
	}
	if (!wait_raft_room(dst)) {
		atomic_inc(&relay_stats.raft_late); /* relayed without the reserve */
	}
	uint32_t start = k_cycle_get_32();

	tp_toggle(TP_RELAY_TX);
	for (;;) {
		int ret = canbus_forward_try(dst, &item->frame, item->t_rx,
					     K_USEC(RELAY_RAFT_TRY_US));

		if (ret == 0) {
			atomic_inc(&relay_stats.forwarded);
			return;
		}
		if (ret != -EAGAIN) {
			return; /* bus down or degraded meanwhile; counted by canbus */
		}
		if (k_cyc_to_us_floor32(k_cycle_get_32() - start) >= RELAY_RAFT_TX_WAIT_US) {
			atomic_inc(&relay_stats.raft_lost);
			canbus_note_tx_full(dst);
			return;
		}
		forward_waiting_urgent();
	}
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

		bool raft_bulk = catalog_is_raft(item.frame.id) &&
				 (item.frame.id & ~3u) != ID_REF(0);

		if (raft_bulk) {
			forward_raft(&item);
		} else {
			forward(&item);
		}
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
