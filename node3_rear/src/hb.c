#include "hb.h"

#include <zephyr/kernel.h>

#include "canbus.h"
#include "lab3_config.h"
#include "node_config.h"
#include "tp.h"
#include "jitter.h"
#include "watchdog.h"
#include "wcet.h"
#include "zone.h"

#define HB_STACK_SIZE 1024

static const enum tp_id tp_deg[NUM_BUSES] = {TP_DEG_A, TP_DEG_B};

static atomic_t deg[NUM_BUSES] = {ATOMIC_INIT(1), ATOMIC_INIT(1)};
static atomic_t peer_alive[NUM_NODES];
static atomic_t relay_active;

K_SEM_DEFINE(hb_due, 0, 1);

static atomic_t hb_release; /* k_cycle_get_32() at the last timer expiry: J's start */

static void hb_timer_fn(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	atomic_set(&hb_release, (atomic_val_t)jit_now());
	k_sem_give(&hb_due);
}
K_TIMER_DEFINE(hb_timer, hb_timer_fn, NULL);

K_THREAD_STACK_DEFINE(hb_tx_stack, HB_STACK_SIZE);
K_THREAD_STACK_DEFINE(hb_mon_stack, HB_STACK_SIZE);
static struct k_thread hb_tx_data, hb_mon_data;

const char *hb_state_name(enum zone_state state)
{
	return zone_state_name(state);
}

bool hb_bus_degraded(int bus)
{
	return atomic_get(&deg[bus]) != 0;
}

bool hb_peer_alive(int node)
{
	return atomic_get(&peer_alive[node]) != 0;
}

enum zone_state hb_zone_state(void)
{
	return zone_state();
}

void hb_set_relay_active(bool active)
{
	atomic_set(&relay_active, active ? 1 : 0);
}

static void hb_tx_thread(void *p1, void *p2, void *p3)
{
	struct wcet_job job = WCET_JOB(WCET_HB_TX);

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		uint8_t p[FRAME_DLC] = {0};

		wcet_end(&job);
		k_sem_take(&hb_due, K_FOREVER);
		wcet_begin(&job);

		/*
		 * On a degraded bus the previous heartbeat is probably still sitting
		 * in a TX buffer, retrying with no ACK. Drop it so that when the link
		 * comes back, what goes out first is current, not a stale frame.
		 */
		for (int bus = 0; bus < NUM_BUSES; bus++) {
			if (hb_bus_degraded(bus)) {
				canbus_flush(bus);
			}
		}

		uint8_t lf = zone_local_fault();

		p[1] = (uint8_t)((zone_state() & HB_B1_STATE_MASK) | (lf ? HB_B1_LOCAL_FAULT : 0));
		p[2] = (hb_bus_degraded(BUS_A) ? HB_B2_A_DEGRADED : 0) |
		       (hb_bus_degraded(BUS_B) ? HB_B2_B_DEGRADED : 0) |
		       (canbus_take_busoff(BUS_A) ? HB_B2_A_BUSOFF : 0) |
		       (canbus_take_busoff(BUS_B) ? HB_B2_B_BUSOFF : 0) |
		       (atomic_get(&relay_active) ? HB_B2_RELAY : 0);
		p[3] = zone_last_fault();

		/* Heartbeats go out on degraded buses too: that's how the link recovers. */
		canbus_send_at(ID_HB(NODE_SELF), p, true, (uint32_t)atomic_get(&hb_release));
		watchdog_checkin(WDT_HB_TX);
	}
}

static bool heard_within(int bus, int node, uint32_t now, uint32_t timeout_ms)
{
	uint32_t t = canbus_last_hb_ms(bus, node);

	return t != 0 && (uint32_t)(now - t) <= timeout_ms;
}

static void hb_mon_thread(void *p1, void *p2, void *p3)
{
	int64_t next = k_uptime_get();
	struct wcet_job job = WCET_JOB(WCET_HB_MON);

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		wcet_begin(&job);

		uint32_t now = k_uptime_get_32();

		/* Degraded: no other node heard on this bus for DEG_TIMEOUT_MS. */
		for (int bus = 0; bus < NUM_BUSES; bus++) {
			bool heard_any = false;

			for (int node = 0; node < NUM_NODES; node++) {
				if (node != NODE_SELF &&
				    heard_within(bus, node, now, DEG_TIMEOUT_MS)) {
					heard_any = true;
				}
			}
			bool d = !heard_any || !canbus_bus_running(bus);

			if (d != hb_bus_degraded(bus)) {
				atomic_set(&deg[bus], d);
				canbus_set_degraded(bus, d);
				tp_set(tp_deg[bus], d);
				if (d) {
					canbus_flush(bus); /* drop frames queued for a dead link */
				}
			}
		}

		/* Zone lost: heartbeat missing on both buses for FAIL_TIMEOUT_MS. */
		for (int node = 0; node < NUM_NODES; node++) {
			if (node != NODE_SELF) {
				atomic_set(&peer_alive[node],
					   heard_within(BUS_A, node, now, FAIL_TIMEOUT_MS) ||
						   heard_within(BUS_B, node, now, FAIL_TIMEOUT_MS));
			}
		}

		zone_update();
		watchdog_service();
		wcet_end(&job);

		next += MONITOR_PERIOD_MS;
		k_sleep(K_TIMEOUT_ABS_MS(next));
	}
}

int hb_init(void)
{
	k_thread_create(&hb_mon_data, hb_mon_stack, K_THREAD_STACK_SIZEOF(hb_mon_stack),
			hb_mon_thread, NULL, NULL, NULL, PRIO_HB_MON, 0, K_NO_WAIT);
	k_thread_name_set(&hb_mon_data, "hb_mon");
	k_thread_create(&hb_tx_data, hb_tx_stack, K_THREAD_STACK_SIZEOF(hb_tx_stack),
			hb_tx_thread, NULL, NULL, NULL, PRIO_HB_TX, 0, K_NO_WAIT);
	k_thread_name_set(&hb_tx_data, "hb_tx");
	k_timer_start(&hb_timer, K_NO_WAIT, K_MSEC(HB_PERIOD_MS));
	return 0;
}
