#include "relay.h"

#include <zephyr/kernel.h>

#include "canbus.h"
#include "hb.h"
#include "lab3_config.h"
#include "node_config.h"
#include "tp.h"
#include "wcet.h"

struct relay_stats relay_stats;

#if RELAY_ENABLED

#define RELAY_STACK_SIZE  1024
#define RELAY_QUEUE_DEPTH 16

struct relay_item {
	uint8_t src_bus;
	uint32_t t_rx; /* rx_cb time: J's start */
	struct can_frame frame;
};
K_MSGQ_DEFINE(relay_q, sizeof(struct relay_item), RELAY_QUEUE_DEPTH, 4);

K_THREAD_STACK_DEFINE(relay_stack, RELAY_STACK_SIZE);
static struct k_thread relay_data;

/* Dispatch thread, first copy only: hand it to RELAY without waiting. */
static void relay_hook(const struct can_frame *frame, int bus)
{
	struct relay_item item = {
		.src_bus = (uint8_t)bus, .t_rx = canbus_rx_cycles(), .frame = *frame};

	if (k_msgq_put(&relay_q, &item, K_NO_WAIT) != 0) {
		atomic_inc(&relay_stats.dropped);
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
		k_msgq_get(&relay_q, &item, K_FOREVER);
		wcet_begin(&job);

		int dst = item.src_bus == BUS_A ? BUS_B : BUS_A;

		if (canbus_is_degraded(dst) || !canbus_bus_running(dst)) {
			atomic_inc(&relay_stats.skipped);
			continue;
		}
		tp_toggle(TP_RELAY_TX);
		if (canbus_forward_at(dst, &item.frame, item.t_rx) == 0) {
			atomic_inc(&relay_stats.forwarded);
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
