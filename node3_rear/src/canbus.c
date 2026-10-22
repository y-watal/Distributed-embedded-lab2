#include "canbus.h"

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "lab3_config.h"
#include "node_config.h"
#include "tp.h"
#include "jitter.h"
#include "wcet.h"

#define DISPATCH_STACK_SIZE 1536
#define RX_QUEUE_DEPTH      32

static const struct device *const can_devs[NUM_BUSES] = {
	DEVICE_DT_GET(DT_ALIAS(can_a)),
	DEVICE_DT_GET(DT_ALIAS(can_b)),
};
static const char bus_name[NUM_BUSES] = {'A', 'B'};
static const enum tp_id tp_rx[NUM_BUSES] = {TP_CAN_RX_A, TP_CAN_RX_B};
static const enum tp_id tp_tx[NUM_BUSES] = {TP_CAN_TX_A, TP_CAN_TX_B};

const uint16_t canbus_slot_id[CANBUS_NUM_SLOTS] = {
	ID_CTRL,
	ID_FAULT(NODE_N1), ID_FAULT(NODE_N2), ID_FAULT(NODE_N3),
	ID_HB(NODE_N1), ID_HB(NODE_N2), ID_HB(NODE_N3),
	ID_BLINK,
	ID_FORCE,
};

/* Hardware filters: masks cover the FAULT and HB ranges in one filter each. */
static const struct can_filter catalog_filters[] = {
	{.id = ID_CTRL, .mask = CAN_STD_ID_MASK},
	{.id = ID_FAULT(0), .mask = 0x7FC}, /* 0x020-0x023 */
	{.id = ID_HB(0), .mask = 0x7FC},    /* 0x100-0x103 */
	{.id = ID_BLINK, .mask = CAN_STD_ID_MASK},
	{.id = ID_FORCE, .mask = CAN_STD_ID_MASK},
};

struct canbus_stats canbus_stats;

static bool bus_running[NUM_BUSES];
static atomic_t bus_degraded[NUM_BUSES];
static atomic_t bus_busoff[NUM_BUSES];

static canbus_handler_t relay_hook;
static uint32_t cur_rx_t; /* dispatch thread: t_rx of the frame being handled */
static canbus_handler_t app_handler;

/* Per-ID send sequence counters. */
static uint8_t tx_seq[CANBUS_NUM_SLOTS];
static struct k_spinlock tx_lock;

/* HB_TX and HB_MON can both flush a bus; one stop/start at a time. */
K_MUTEX_DEFINE(flush_lock);

/* Dispatch thread state (only the dispatch thread writes these). */
struct seen {
	int64_t ms;
	uint8_t seq;
	bool valid;
};
static struct seen seen[CANBUS_NUM_SLOTS];
/* 32-bit so other threads read it in one access; 0 = never heard. */
static uint32_t last_hb_ms[NUM_BUSES][NUM_NODES];

struct rx_item {
	uint8_t bus;
	uint32_t t_rx; /* k_cycle_get_32() in rx_cb */
	struct can_frame frame;
};
K_MSGQ_DEFINE(rx_q, sizeof(struct rx_item), RX_QUEUE_DEPTH, 4);

K_THREAD_STACK_DEFINE(dispatch_stack, DISPATCH_STACK_SIZE);
static struct k_thread dispatch_thread_data;

static int slot_of(uint32_t id)
{
	for (int i = 0; i < CANBUS_NUM_SLOTS; i++) {
		if (canbus_slot_id[i] == id) {
			return i;
		}
	}
	return -1;
}

const struct device *canbus_dev(int bus)
{
	return can_devs[bus];
}

/* MCP2515 driver thread, right after the SPI read: no printing, just queue it. */
static void rx_cb(const struct device *dev, struct can_frame *frame, void *user_data)
{
	struct rx_item item = {.bus = (uint8_t)(uintptr_t)user_data, .frame = *frame};

	ARG_UNUSED(dev);
	item.t_rx = jit_now();
	tp_toggle(tp_rx[item.bus]);
	wcet_driver_seen(item.bus);
	if (k_msgq_put(&rx_q, &item, K_NO_WAIT) != 0) {
		atomic_inc(&canbus_stats.rx_dropped);
	}
}

static void tx_cb(const struct device *dev, int error, void *user_data)
{
	int bus = (int)(uintptr_t)user_data;

	ARG_UNUSED(dev);
	atomic_inc(error == 0 ? &canbus_stats.bus[bus].tx_ok : &canbus_stats.bus[bus].tx_err);
}

static void state_cb(const struct device *dev, enum can_state state, struct can_bus_err_cnt err,
		     void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(err);
	if (state == CAN_STATE_BUS_OFF) {
		atomic_set(&bus_busoff[(int)(uintptr_t)user_data], 1);
	}
}

static void dispatch_thread(void *p1, void *p2, void *p3)
{
	struct rx_item item;
	struct wcet_job job = WCET_JOB(WCET_DISPATCH);

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		wcet_end(&job);
		k_msgq_get(&rx_q, &item, K_FOREVER);
		wcet_begin(&job);

		const struct can_frame *f = &item.frame;

		job.slot = f->id == ID_CTRL ? WCET_DISPATCH_CTRL : WCET_DISPATCH;
		int slot = slot_of(f->id);
		int sender = catalog_sender(f->id);
		int64_t now = k_uptime_get();

		if (slot < 0 || f->dlc != FRAME_DLC) {
			atomic_inc(&canbus_stats.unknown_id);
			continue;
		}
		atomic_inc(&canbus_stats.bus[item.bus].rx[slot]);

		/* Our own frame, relayed back to us by N2. */
		if (sender == NODE_SELF) {
			atomic_inc(&canbus_stats.own_echo);
			continue;
		}

		/* Presence counts every copy: a relayed heartbeat still means "alive". */
		if (catalog_is_hb(f->id)) {
			uint32_t t = (uint32_t)now;

			last_hb_ms[item.bus][sender] = t != 0 ? t : 1;
		}

		uint8_t seq = f->data[0] & SEQ_MASK;
		struct seen *s = &seen[slot];

		if (s->valid && s->seq == seq && now - s->ms < DUP_WINDOW_MS) {
			atomic_inc(&canbus_stats.duplicates);
			continue;
		}
		if (s->valid && seq != ((s->seq + 1) & SEQ_MASK)) {
			atomic_add(&canbus_stats.gaps[slot], (seq - s->seq - 1) & SEQ_MASK);
		}
		s->seq = seq;
		s->ms = now;
		s->valid = true;
		atomic_inc(&canbus_stats.accepted[slot]);

		cur_rx_t = item.t_rx;
		if (relay_hook) {
			relay_hook(f, item.bus);
		}
		if (app_handler) {
			app_handler(f, item.bus);
		}
	}
}

uint32_t canbus_rx_cycles(void)
{
	return cur_rx_t;
}

static int queue_on(int bus, const struct can_frame *frame, uint32_t release, bool relayed)
{
	int ret;

	tp_toggle(tp_tx[bus]);
	ret = can_send(can_devs[bus], frame, K_NO_WAIT, tx_cb, (void *)(uintptr_t)bus);
	if (ret == 0) {
		jit_note(frame->id, bus, release, relayed);
	}
	if (ret == -EAGAIN) {
		atomic_inc(&canbus_stats.bus[bus].tx_full); /* all 3 TX buffers busy */
	} else if (ret < 0) {
		atomic_inc(&canbus_stats.bus[bus].tx_err);
	}
	return ret;
}

int canbus_send(uint32_t id, uint8_t payload[FRAME_DLC], bool include_degraded)
{
	return canbus_send_at(id, payload, include_degraded, 0);
}

int canbus_send_at(uint32_t id, uint8_t payload[FRAME_DLC], bool include_degraded,
		   uint32_t release)
{
	int slot = slot_of(id);
	struct can_frame frame = {.id = id, .dlc = FRAME_DLC};
	int sent = 0;

	if (slot < 0) {
		return -EINVAL;
	}

	k_spinlock_key_t key = k_spin_lock(&tx_lock);
	payload[0] = tx_seq[slot] & SEQ_MASK; /* flags nibble: none defined */
	tx_seq[slot] = (tx_seq[slot] + 1) & SEQ_MASK;
	k_spin_unlock(&tx_lock, key);

	memcpy(frame.data, payload, FRAME_DLC);

	for (int bus = 0; bus < NUM_BUSES; bus++) {
		if (!bus_running[bus]) {
			continue;
		}
		if (!include_degraded && atomic_get(&bus_degraded[bus])) {
			atomic_inc(&canbus_stats.bus[bus].tx_skipped);
			continue;
		}
		if (queue_on(bus, &frame, release, false) == 0) {
			sent++;
		}
	}
	return sent > 0 ? 0 : -EIO;
}

int canbus_forward(int bus, const struct can_frame *frame)
{
	return canbus_forward_at(bus, frame, 0);
}

int canbus_forward_at(int bus, const struct can_frame *frame, uint32_t release)
{
	if (!bus_running[bus]) {
		return -ENETDOWN;
	}
	if (atomic_get(&bus_degraded[bus])) {
		atomic_inc(&canbus_stats.bus[bus].tx_skipped);
		return -ENETUNREACH;
	}
	return queue_on(bus, frame, release, true);
}

void canbus_flush(int bus)
{
	if (!bus_running[bus]) {
		return;
	}
	/* can_stop() aborts pending transmissions; restart keeps the filters. */
	k_mutex_lock(&flush_lock, K_FOREVER);
	can_stop(can_devs[bus]);
	can_start(can_devs[bus]);
	k_mutex_unlock(&flush_lock);
}

bool canbus_bus_running(int bus)
{
	return bus_running[bus];
}

void canbus_set_degraded(int bus, bool degraded)
{
	atomic_set(&bus_degraded[bus], degraded ? 1 : 0);
}

bool canbus_is_degraded(int bus)
{
	return atomic_get(&bus_degraded[bus]) != 0;
}

uint32_t canbus_last_hb_ms(int bus, int node)
{
	return last_hb_ms[bus][node];
}

bool canbus_take_busoff(int bus)
{
	return atomic_clear(&bus_busoff[bus]) != 0;
}

void canbus_set_relay_hook(canbus_handler_t hook)
{
	relay_hook = hook;
}

void canbus_set_handler(canbus_handler_t handler)
{
	app_handler = handler;
}

static int setup_bus(int bus)
{
	const struct device *dev = can_devs[bus];
	int ret;

	if (!device_is_ready(dev)) {
		printk("CAN %c (%s): not ready - check SPI wiring, CS pin and osc-freq\n",
		       bus_name[bus], dev->name);
		return -ENODEV;
	}

	for (size_t i = 0; i < ARRAY_SIZE(catalog_filters); i++) {
		ret = can_add_rx_filter(dev, rx_cb, (void *)(uintptr_t)bus, &catalog_filters[i]);
		if (ret < 0) {
			printk("CAN %c: can_add_rx_filter failed (%d)\n", bus_name[bus], ret);
			return ret;
		}
	}
	/*
	 * Deprecated in Zephyr 4.5 in favour of can_add_state_change_callback(),
	 * which older 4.4 trees don't have yet; this one works in both.
	 */
	can_set_state_change_callback(dev, state_cb, (void *)(uintptr_t)bus);

	ret = can_start(dev);
	if (ret < 0) {
		printk("CAN %c: can_start failed (%d)\n", bus_name[bus], ret);
		return ret;
	}
	printk("CAN %c (%s): started\n", bus_name[bus], dev->name);
	return 0;
}

int canbus_init(void)
{
	int up = 0;

	k_thread_create(&dispatch_thread_data, dispatch_stack,
			K_THREAD_STACK_SIZEOF(dispatch_stack), dispatch_thread, NULL, NULL, NULL,
			PRIO_DISPATCH, 0, K_NO_WAIT);
	k_thread_name_set(&dispatch_thread_data, "dispatch");

	for (int bus = 0; bus < NUM_BUSES; bus++) {
		bus_running[bus] = (setup_bus(bus) == 0);
		atomic_set(&bus_degraded[bus], 1); /* until a heartbeat is heard */
		up += bus_running[bus];
	}
	return up > 0 ? 0 : -ENODEV;
}
