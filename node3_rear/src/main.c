/*
 * Lab 3 CAN bring-up test with test points.
 *
 * Every 20 ms this node sends its heartbeat ID on both buses, with one shared
 * 4-bit sequence counter in the low nibble of byte 0. Every received frame is
 * counted per bus and per ID, and once a second a summary is printed on the
 * console (USART2, the ST-LINK virtual COM port, 115200 baud):
 *
 *   [N3 12.000s] rx dropped 0 | ERR 0
 *     A: error-active tec=0 rec=0 DEG 0 | tx ok=50 err=0 full=0 | rx: 0x100 x50 0x101 x50
 *     B: error-active tec=0 rec=0 DEG 0 | tx ok=50 err=0 full=0 | rx: 0x100 x50 0x101 x50
 *
 * "gaps" after an ID counts sequence numbers that were skipped (lost frames).
 *
 * Test points (src/tp.c, docs/lab3_pin_map.md):
 *   CAN_TX_A/B  toggle on each can_send() call to that controller
 *   CAN_RX_A/B  toggle on each frame the driver hands over from that controller
 *   DEG_A/B     on while no other node's heartbeat has been heard on that bus
 *               for HB_TIMEOUT_MS (placeholder for HB_MON, R4.7)
 *   ERR         on at power-up, and while any other node is missing on both
 *               buses for HB_TIMEOUT_MS (placeholder for the error state, R4.5)
 *   PWM         held at PWM_IDLE_US
 *   others      only driven by the boot-time walk (TP_WALK_MS each)
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#include "tp.h"

/* ---- Node identity: the only lines that differ between N2 and N3 ---- */
#define NODE_NAME   "N3"
#define HB_ID       0x102 /* HB_N3 in the message catalog */
#define PWM_IDLE_US 0     /* PWM_OUT: motor off */
/* --------------------------------------------------------------------- */

#define HB_BASE_ID       0x100 /* HB_N1; HB_N2 = 0x101, HB_N3 = 0x102 */
#define NUM_NODES        3
#define HB_PERIOD_MS     20
#define HB_TIMEOUT_MS    (3 * HB_PERIOD_MS) /* three missed heartbeats */
#define MONITOR_MS       5
#define REPORT_PERIOD_MS 1000
#define TP_WALK_MS       300 /* 0 skips the boot-time test point walk */
#define NUM_BUSES        2
#define MAX_IDS          16
#define RX_STACK_SIZE    2048
#define RX_PRIORITY      7

static const char bus_name[NUM_BUSES] = {'A', 'B'};
static const enum tp_id tp_rx[NUM_BUSES] = {TP_CAN_RX_A, TP_CAN_RX_B};
static const enum tp_id tp_tx[NUM_BUSES] = {TP_CAN_TX_A, TP_CAN_TX_B};
static const enum tp_id tp_deg[NUM_BUSES] = {TP_DEG_A, TP_DEG_B};

static const struct device *const can_dev[NUM_BUSES] = {
	DEVICE_DT_GET(DT_ALIAS(can_a)),
	DEVICE_DT_GET(DT_ALIAS(can_b)),
};

static bool bus_up[NUM_BUSES];

struct rx_item {
	uint8_t bus;
	struct can_frame frame;
};

K_MSGQ_DEFINE(rx_q, sizeof(struct rx_item), 32, 4);

static atomic_t rx_dropped;
static atomic_t tx_ok[NUM_BUSES];
static atomic_t tx_err[NUM_BUSES];
static atomic_t tx_full[NUM_BUSES];

/* Receive statistics and heartbeat times, owned by the RX thread. */
struct id_stats {
	uint16_t id;
	uint16_t count;
	uint16_t gaps;
	uint8_t last_seq;
	bool have_seq;
};

static struct id_stats stats[NUM_BUSES][MAX_IDS];
static int n_ids[NUM_BUSES];
static int64_t last_hb_ms[NUM_BUSES][NUM_NODES]; /* 0 = never heard */
static bool deg[NUM_BUSES] = {true, true};
static bool err_state = true;

K_THREAD_STACK_DEFINE(rx_stack, RX_STACK_SIZE);
static struct k_thread rx_thread_data;

/* Runs in the MCP2515 driver's thread right after the SPI read: no printing. */
static void rx_cb(const struct device *dev, struct can_frame *frame, void *user_data)
{
	struct rx_item item = {
		.bus = (uint8_t)(uintptr_t)user_data,
		.frame = *frame,
	};

	ARG_UNUSED(dev);
	tp_toggle(tp_rx[item.bus]);
	if (k_msgq_put(&rx_q, &item, K_NO_WAIT) != 0) {
		atomic_inc(&rx_dropped);
	}
}

static void tx_cb(const struct device *dev, int error, void *user_data)
{
	int bus = (int)(uintptr_t)user_data;

	ARG_UNUSED(dev);
	atomic_inc(error == 0 ? &tx_ok[bus] : &tx_err[bus]);
}

static const char *state_str(enum can_state state)
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

static void account(const struct rx_item *item)
{
	struct id_stats *tab = stats[item->bus];
	struct id_stats *s = NULL;
	uint32_t id = item->frame.id;

	if (id >= HB_BASE_ID && id < HB_BASE_ID + NUM_NODES && id != HB_ID) {
		last_hb_ms[item->bus][id - HB_BASE_ID] = k_uptime_get();
	}

	for (int i = 0; i < n_ids[item->bus]; i++) {
		if (tab[i].id == id) {
			s = &tab[i];
			break;
		}
	}
	if (s == NULL) {
		if (n_ids[item->bus] == MAX_IDS) {
			return;
		}
		s = &tab[n_ids[item->bus]++];
		*s = (struct id_stats){.id = (uint16_t)id};
	}

	s->count++;
	if (item->frame.dlc > 0) {
		uint8_t seq = item->frame.data[0] & 0x0F;

		if (s->have_seq && seq != ((s->last_seq + 1) & 0x0F)) {
			s->gaps++;
		}
		s->last_seq = seq;
		s->have_seq = true;
	}
}

static bool heard_recently(int bus, int node, int64_t now)
{
	return last_hb_ms[bus][node] != 0 && now - last_hb_ms[bus][node] <= HB_TIMEOUT_MS;
}

/* Placeholder HB_MON: drives DEG_A/B and ERR from heartbeat ages. */
static void monitor(void)
{
	int64_t now = k_uptime_get();
	const int self = HB_ID - HB_BASE_ID;
	bool any_missing = false;

	for (int bus = 0; bus < NUM_BUSES; bus++) {
		bool heard_any = false;

		for (int node = 0; node < NUM_NODES; node++) {
			if (node != self && heard_recently(bus, node, now)) {
				heard_any = true;
			}
		}
		deg[bus] = !heard_any;
		tp_set(tp_deg[bus], deg[bus]);
	}

	for (int node = 0; node < NUM_NODES; node++) {
		if (node != self && !heard_recently(0, node, now) && !heard_recently(1, node, now)) {
			any_missing = true;
		}
	}
	err_state = any_missing;
	tp_set(TP_ERR, err_state);
}

static void report(void)
{
	uint32_t ms = k_uptime_get_32();

	printk("[%s %u.%03us] rx dropped %ld | ERR %d\n", NODE_NAME, ms / 1000, ms % 1000,
	       (long)atomic_clear(&rx_dropped), err_state);

	for (int bus = 0; bus < NUM_BUSES; bus++) {
		if (!bus_up[bus]) {
			printk("  %c: not running\n", bus_name[bus]);
			continue;
		}

		enum can_state state;
		struct can_bus_err_cnt err = {0};

		can_get_state(can_dev[bus], &state, &err);
		printk("  %c: %s tec=%u rec=%u DEG %d | tx ok=%ld err=%ld full=%ld | rx:",
		       bus_name[bus], state_str(state), err.tx_err_cnt, err.rx_err_cnt, deg[bus],
		       (long)atomic_clear(&tx_ok[bus]), (long)atomic_clear(&tx_err[bus]),
		       (long)atomic_clear(&tx_full[bus]));

		if (n_ids[bus] == 0) {
			printk(" (nothing yet)");
		}
		for (int i = 0; i < n_ids[bus]; i++) {
			struct id_stats *s = &stats[bus][i];

			printk(" 0x%03x x%u", s->id, s->count);
			if (s->gaps) {
				printk(" (gaps %u)", s->gaps);
			}
			s->count = 0;
			s->gaps = 0;
		}
		printk("\n");
	}
}

static void rx_thread(void *p1, void *p2, void *p3)
{
	int64_t next_report = k_uptime_get() + REPORT_PERIOD_MS;
	int64_t next_monitor = k_uptime_get() + MONITOR_MS;
	struct rx_item item;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		int64_t wake = MIN(next_report, next_monitor);

		if (k_msgq_get(&rx_q, &item, K_TIMEOUT_ABS_MS(wake)) == 0) {
			account(&item);
		}
		int64_t now = k_uptime_get();

		if (now >= next_monitor) {
			monitor();
			next_monitor += MONITOR_MS;
		}
		if (now >= next_report) {
			report();
			next_report += REPORT_PERIOD_MS;
		}
	}
}

static int setup_bus(int bus)
{
	const struct device *dev = can_dev[bus];
	/* Mask 0 accepts every standard-ID frame. */
	const struct can_filter all_std = {.id = 0, .mask = 0, .flags = 0};
	int ret;

	if (!device_is_ready(dev)) {
		printk("CAN %c (%s): not ready - check SPI wiring, CS pin and osc-freq\n",
		       bus_name[bus], dev->name);
		return -ENODEV;
	}

	ret = can_add_rx_filter(dev, rx_cb, (void *)(uintptr_t)bus, &all_std);
	if (ret < 0) {
		printk("CAN %c: can_add_rx_filter failed (%d)\n", bus_name[bus], ret);
		return ret;
	}

	ret = can_start(dev);
	if (ret < 0) {
		printk("CAN %c: can_start failed (%d)\n", bus_name[bus], ret);
		return ret;
	}

	printk("CAN %c (%s): started\n", bus_name[bus], dev->name);
	return 0;
}

int main(void)
{
	uint8_t seq = 0;
	int64_t next_tx;

	printk("Lab 3 CAN test, node %s, heartbeat ID 0x%03x every %d ms\n", NODE_NAME, HB_ID,
	       HB_PERIOD_MS);

	tp_init();
	if (tp_pwm_present()) {
		tp_pwm_set_pulse_us(PWM_IDLE_US);
	}
	if (TP_WALK_MS > 0) {
		tp_selftest(TP_WALK_MS);
	}
	/* Power up in the error state with both buses degraded until heartbeats arrive. */
	tp_set(TP_ERR, 1);
	tp_set(TP_DEG_A, 1);
	tp_set(TP_DEG_B, 1);

	for (int bus = 0; bus < NUM_BUSES; bus++) {
		bus_up[bus] = (setup_bus(bus) == 0);
	}

	k_thread_create(&rx_thread_data, rx_stack, K_THREAD_STACK_SIZEOF(rx_stack), rx_thread,
			NULL, NULL, NULL, RX_PRIORITY, 0, K_NO_WAIT);

	next_tx = k_uptime_get();
	for (;;) {
		struct can_frame frame = {
			.id = HB_ID,
			.dlc = 8,
		};

		frame.data[0] = seq & 0x0F; /* low nibble: sequence, high nibble: flags (none) */
		frame.data[1] = 0;          /* zone state: unused in this test */
		frame.data[2] = (bus_up[0] ? 0x01 : 0) | (bus_up[1] ? 0x02 : 0); /* bus A/B health */

		/* Queue on both controllers back to back; never block on a missing ACK. */
		for (int bus = 0; bus < NUM_BUSES; bus++) {
			if (!bus_up[bus]) {
				continue;
			}
			tp_toggle(tp_tx[bus]);
			int ret = can_send(can_dev[bus], &frame, K_NO_WAIT, tx_cb,
					   (void *)(uintptr_t)bus);
			if (ret == -EAGAIN) {
				atomic_inc(&tx_full[bus]); /* all 3 TX buffers busy */
			} else if (ret < 0) {
				atomic_inc(&tx_err[bus]);
			}
		}

		seq = (seq + 1) & 0x0F;
		next_tx += HB_PERIOD_MS;
		k_sleep(K_TIMEOUT_ABS_MS(next_tx));
	}

	return 0;
}
