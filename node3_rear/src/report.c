#include "report.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "app.h"
#include "canbus.h"
#include "ctrl.h"
#include "hb.h"
#include "lab3_config.h"
#include "node_config.h"
#include "relay.h"

#define REPORT_STACK_SIZE 1536

K_THREAD_STACK_DEFINE(report_stack, REPORT_STACK_SIZE);
static struct k_thread report_data;

static const char *can_state_name(enum can_state state)
{
	switch (state) {
	case CAN_STATE_ERROR_ACTIVE:
		return "error-active";
	case CAN_STATE_ERROR_WARNING:
		return "error-warning";
	case CAN_STATE_ERROR_PASSIVE:
		return "error-passive";
	case CAN_STATE_BUS_OFF:
		return "bus-off";
	case CAN_STATE_STOPPED:
		return "stopped";
	default:
		return "?";
	}
}

static void print_counts(atomic_t *counts, atomic_t *gaps)
{
	bool any = false;

	for (int i = 0; i < CANBUS_NUM_SLOTS; i++) {
		long n = (long)atomic_clear(&counts[i]);
		long g = gaps ? (long)atomic_clear(&gaps[i]) : 0;

		if (n == 0 && g == 0) {
			continue;
		}
		printk(" 0x%03x x%ld", canbus_slot_id[i], n);
		if (g) {
			printk(" (gaps %ld)", g);
		}
		any = true;
	}
	if (!any) {
		printk(" (nothing)");
	}
}

static void report_thread(void *p1, void *p2, void *p3)
{
	int64_t next = k_uptime_get() + REPORT_PERIOD_MS;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		k_sleep(K_TIMEOUT_ABS_MS(next));
		next += REPORT_PERIOD_MS;

		uint32_t ms = k_uptime_get_32();
		struct canbus_stats *s = &canbus_stats;

		printk("[%s %u.%03us] %s | peers:", NODE_NAME, ms / 1000, ms % 1000,
		       hb_state_name(hb_zone_state()));
		for (int n = 0; n < NUM_NODES; n++) {
			if (n != NODE_SELF) {
				printk(" N%d %s", n + 1, hb_peer_alive(n) ? "ok" : "LOST");
			}
		}
		printk(" | dup %ld echo %ld rxdrop %ld", (long)atomic_clear(&s->duplicates),
		       (long)atomic_clear(&s->own_echo), (long)atomic_clear(&s->rx_dropped));
		if (RELAY_ENABLED) {
			printk(" | relay %ld skip %ld drop %ld",
			       (long)atomic_clear(&relay_stats.forwarded),
			       (long)atomic_clear(&relay_stats.skipped),
			       (long)atomic_clear(&relay_stats.dropped));
		}
		printk("\n");

		for (int bus = 0; bus < NUM_BUSES; bus++) {
			struct canbus_bus_stats *b = &s->bus[bus];

			if (!canbus_bus_running(bus)) {
				printk("  %c: not running\n", 'A' + bus);
				continue;
			}
			enum can_state st;
			struct can_bus_err_cnt err = {0};

			can_get_state(canbus_dev(bus), &st, &err);
			printk("  %c: %s tec=%u rec=%u DEG %d | tx ok=%ld err=%ld full=%ld skip=%ld | rx:",
			       'A' + bus, can_state_name(st), err.tx_err_cnt, err.rx_err_cnt,
			       hb_bus_degraded(bus), (long)atomic_clear(&b->tx_ok),
			       (long)atomic_clear(&b->tx_err), (long)atomic_clear(&b->tx_full),
			       (long)atomic_clear(&b->tx_skipped));
			print_counts(b->rx, NULL);
			printk("\n");
		}
		printk("  accepted:");
		print_counts(s->accepted, s->gaps);
		printk("\n");

		struct ctrl_cmd cmd;
		uint32_t age;

		printk("  ctrl: ok %ld malformed %ld range %ld", (long)atomic_clear(&ctrl_stats.ok),
		       (long)atomic_clear(&ctrl_stats.malformed),
		       (long)atomic_clear(&ctrl_stats.out_of_range));
		if (ctrl_get(&cmd, &age)) {
			printk(" | last: steer %d throttle %u brake %u buttons 0x%x, %u ms ago",
			       cmd.steer, cmd.throttle, cmd.brake, cmd.buttons, age);
		}
		printk("\n");
		app_report();
	}
}

int report_init(void)
{
	k_thread_create(&report_data, report_stack, K_THREAD_STACK_SIZEOF(report_stack),
			report_thread, NULL, NULL, NULL, PRIO_REPORT, 0, K_NO_WAIT);
	k_thread_name_set(&report_data, "report");
	return 0;
}
