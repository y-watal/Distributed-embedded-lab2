/*
 * Lab 3 CAN bring-up test.
 *
 * Every 100 ms this node sends its heartbeat ID on both buses, with one
 * shared 4-bit sequence counter in the low nibble of byte 0. Every received
 * frame is counted per bus and per ID, and once a second a summary is
 * printed on the console (USART2, the ST-LINK virtual COM port):
 *
 *   [N3 12.000s] rx dropped 0
 *     A: error-active tec=0 rec=0 | tx ok=10 err=0 full=0 | rx: 0x100 x10 0x101 x10
 *     B: error-active tec=0 rec=0 | tx ok=10 err=0 full=0 | rx: 0x100 x10 0x101 x10
 *
 * "gaps" after an ID counts sequence numbers that were skipped (lost frames).
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

/* ---- Node identity: the only lines that differ between N2 and N3 ---- */
#define NODE_NAME "N3"
#define HB_ID     0x102 /* HB_N3 in the message catalog */
/* --------------------------------------------------------------------- */

#define TX_PERIOD_MS     100
#define REPORT_PERIOD_MS 1000
#define NUM_BUSES        2
#define MAX_IDS          16
#define RX_STACK_SIZE    2048
#define RX_PRIORITY      7

static const char bus_name[NUM_BUSES] = {'A', 'B'};

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

/* Per-bus receive statistics, owned by the RX thread. */
struct id_stats {
	uint16_t id;
	uint16_t count;
	uint16_t gaps;
	uint8_t last_seq;
	bool have_seq;
};

static struct id_stats stats[NUM_BUSES][MAX_IDS];
static int n_ids[NUM_BUSES];

K_THREAD_STACK_DEFINE(rx_stack, RX_STACK_SIZE);
static struct k_thread rx_thread_data;

/* Runs in the MCP2515 driver's interrupt thread: no printing, just queue it. */
static void rx_cb(const struct device *dev, struct can_frame *frame, void *user_data)
{
	struct rx_item item = {
		.bus = (uint8_t)(uintptr_t)user_data,
		.frame = *frame,
	};

	ARG_UNUSED(dev);
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

	for (int i = 0; i < n_ids[item->bus]; i++) {
		if (tab[i].id == item->frame.id) {
			s = &tab[i];
			break;
		}
	}
	if (s == NULL) {
		if (n_ids[item->bus] == MAX_IDS) {
			return;
		}
		s = &tab[n_ids[item->bus]++];
		*s = (struct id_stats){.id = (uint16_t)item->frame.id};
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

static void report(void)
{
	uint32_t ms = k_uptime_get_32();

	printk("[%s %u.%03us] rx dropped %ld\n", NODE_NAME, ms / 1000, ms % 1000,
	       (long)atomic_clear(&rx_dropped));

	for (int bus = 0; bus < NUM_BUSES; bus++) {
		if (!bus_up[bus]) {
			printk("  %c: not running\n", bus_name[bus]);
			continue;
		}

		enum can_state state;
		struct can_bus_err_cnt err = {0};

		can_get_state(can_dev[bus], &state, &err);
		printk("  %c: %s tec=%u rec=%u | tx ok=%ld err=%ld full=%ld | rx:", bus_name[bus],
		       state_str(state), err.tx_err_cnt, err.rx_err_cnt,
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
	struct rx_item item;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		if (k_msgq_get(&rx_q, &item, K_TIMEOUT_ABS_MS(next_report)) == 0) {
			account(&item);
		}
		if (k_uptime_get() >= next_report) {
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
	       TX_PERIOD_MS);

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
			int ret = can_send(can_dev[bus], &frame, K_NO_WAIT, tx_cb,
					   (void *)(uintptr_t)bus);
			if (ret == -EAGAIN) {
				atomic_inc(&tx_full[bus]); /* all 3 TX buffers busy */
			} else if (ret < 0) {
				atomic_inc(&tx_err[bus]);
			}
		}

		seq = (seq + 1) & 0x0F;
		next_tx += TX_PERIOD_MS;
		k_sleep(K_TIMEOUT_ABS_MS(next_tx));
	}

	return 0;
}
