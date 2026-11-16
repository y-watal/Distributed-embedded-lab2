#include "gtime.h"

#include <zephyr/kernel.h>
#include <time.h> /* clock_settime(), CONFIG_POSIX_TIMERS; Zephyr 4.4.99 standard path */
#include <zephyr/sys/printk.h>

#include "catalog.h"
#include "lab3_config.h"
#include "node_config.h"
#include "raft_node.h"
#include "timesync.h"
#include "tp.h"

#define PPS_STACK_SIZE 1024
#define PPS_SPIN_US    400 /* sleep to this far before the edge, then spin */
#define PPS_PULSE_MS   100
#define PPS_QUEUE_LEN  4

/* Everything below is guarded by lock: dispatch, relay, raft, PPS and report
 * threads all read the clock. Each section is a few microseconds. */
static struct k_spinlock lock;
static struct timesync ts;
static uint32_t last_cyc32;
static uint64_t cyc_high;

struct pps_snapshot {
	uint64_t global_us;
	uint64_t local_us;
	uint64_t system_us; /* CLOCK_REALTIME at the edge */
	int32_t drift_ppb;
	int32_t last_error_us;
	int32_t last_correction_us;
	int32_t drift_since_ref_us;
	int8_t master;
	bool leader;
	bool synced;
	bool drift_valid;
	uint32_t refs, steps, stale;
};
K_MSGQ_DEFINE(pps_q, sizeof(struct pps_snapshot), PPS_QUEUE_LEN, 4);
K_THREAD_STACK_DEFINE(pps_stack, PPS_STACK_SIZE);
static struct k_thread pps_data;

/* Caller holds lock. Must run at least once per 2^32 cycles (51 s at 84 MHz):
 * every REF, every stamp and every PPS edge calls it. */
static uint64_t cycles64_locked(uint32_t now32)
{
	if (now32 < last_cyc32) {
		cyc_high += 1ull << 32;
	}
	last_cyc32 = now32;
	return cyc_high | now32;
}

static uint64_t local_us_locked(void)
{
	return k_cyc_to_us_floor64(cycles64_locked(k_cycle_get_32()));
}

uint64_t gtime_local_us(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	uint64_t us = local_us_locked();

	k_spin_unlock(&lock, key);
	return us;
}

uint64_t global_gettime(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	uint64_t g = timesync_global(&ts, local_us_locked());

	k_spin_unlock(&lock, key);
	return g;
}

/*
 * Handout 6.1, "Updating Zephyr on Followers": set the system clock from the
 * adjusted leader time after each REF. CLOCK_REALTIME then reads global time
 * at kernel-tick resolution; global_gettime() stays the precise source (it
 * applies the drift ratio and microsecond timestamps).
 */
static void update_system_time(void)
{
	uint64_t g = global_gettime();
	struct timespec ts = {
		.tv_sec = (time_t)(g / 1000000u),
		.tv_nsec = (long)(g % 1000000u) * 1000L,
	};

	(void)clock_settime(CLOCK_REALTIME, &ts);
}

static uint64_t system_time_us(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
		return 0;
	}
	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

void gtime_on_ref(const struct can_frame *frame, uint32_t rx_cycles)
{
	int sender = catalog_sender(frame->id);
	int leader;
	bool is_leader;

	raft_node_leader_snapshot(&leader, &is_leader);
	/* Only the current leader is our time master. */
	if (is_leader || sender < 0 || sender != leader || frame->dlc != RAFT_REF_LEN) {
		return;
	}
	struct raft_ref ref;

	raft_ref_decode(frame->data, &ref);

	k_spinlock_key_t key = k_spin_lock(&lock);
	uint32_t now32 = k_cycle_get_32();
	uint64_t now64 = cycles64_locked(now32);
	/* rx_cb ran (now32 - rx_cycles) cycles ago */
	uint64_t rx_us = k_cyc_to_us_floor64(now64 - (uint32_t)(now32 - rx_cycles));
	bool used = timesync_on_ref(&ts, sender, &ref, rx_us);

	k_spin_unlock(&lock, key);
	if (used) {
		tp_toggle(TP_REF_RX);
		update_system_time();
	}
}

void gtime_stamp_ref(uint8_t data[8])
{
	timesync_stamp_frame(data, global_gettime());
}

void gtime_restamp_relayed_ref(struct can_frame *frame)
{
	int leader;
	bool is_leader;

	raft_node_leader_snapshot(&leader, &is_leader);
	if (catalog_sender(frame->id) != leader || !gtime_is_synced()) {
		return; /* not our master: pass it on untouched */
	}
	timesync_stamp_frame(frame->data, global_gettime());
}

bool gtime_is_synced(void)
{
	int leader;
	bool is_leader;

	raft_node_leader_snapshot(&leader, &is_leader);

	k_spinlock_key_t key = k_spin_lock(&lock);
	bool synced = timesync_is_synced(&ts, is_leader, local_us_locked());

	k_spin_unlock(&lock, key);
	return synced;
}

static void pps_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		uint64_t g = global_gettime();
		uint64_t edge = (g / 1000000u + 1u) * 1000000u;
		uint64_t wait_us = edge - g;

		if (wait_us > PPS_SPIN_US) {
			k_sleep(K_USEC(wait_us - PPS_SPIN_US));
		}
		/* Spin onto the edge; the clock may be corrected meanwhile. */
		uint32_t spin_start = k_cycle_get_32();

		while ((g = global_gettime()) < edge &&
		       k_cyc_to_us_floor32(k_cycle_get_32() - spin_start) < 4 * PPS_SPIN_US) {
		}
		if (g < edge || g - edge > 500000u) {
			continue; /* clock stepped back or far forward: no stale pulse */
		}
		tp_set(TP_PPS, 1);

		struct pps_snapshot s;
		int leader;
		bool is_leader;

		raft_node_leader_snapshot(&leader, &is_leader);
		k_spinlock_key_t key = k_spin_lock(&lock);
		uint64_t local = local_us_locked();

		s = (struct pps_snapshot){
			.global_us = timesync_global(&ts, local),
			.local_us = local,
			.drift_ppb = ts.drift_ppb,
			.drift_valid = ts.drift_valid,
			.last_error_us = ts.last_error_us,
			.last_correction_us = ts.last_correction_us,
			.drift_since_ref_us = timesync_drift_since_ref_us(&ts, local),
			.master = ts.master,
			.leader = is_leader,
			.synced = timesync_is_synced(&ts, is_leader, local),
			.refs = ts.refs,
			.steps = ts.steps,
			.stale = ts.stale,
		};
		k_spin_unlock(&lock, key);
		s.system_us = system_time_us();
		(void)k_msgq_put(&pps_q, &s, K_NO_WAIT); /* report drains it */

		k_msleep(PPS_PULSE_MS);
		tp_set(TP_PPS, 0);
	}
}

static void print_us(const char *label, uint64_t us)
{
	printk(" %s=%u.%06u", label, (unsigned)(us / 1000000u), (unsigned)(us % 1000000u));
}

void gtime_report(void)
{
	struct pps_snapshot s;
	static uint32_t prev_refs, prev_stale;

	while (k_msgq_get(&pps_q, &s, K_NO_WAIT) == 0) {
		printk("  SYNC:");
		print_us("global", s.global_us);
		print_us("local", s.local_us);
		print_us("sys", s.system_us);
		if (s.leader) {
			printk(" master=self");
		} else if (s.master >= 0) {
			printk(" master=N%d", s.master + 1);
		} else {
			printk(" master=?");
		}
		/* drift as a ratio (L2-L1)/(F2-F1) - 1 in ppm, with 3 decimals */
		int32_t ppb = s.drift_ppb;
		uint32_t mag = (uint32_t)(ppb < 0 ? -ppb : ppb);

		/* ratio = 1 + ppb / 1e9, printed to 9 decimals */
		uint32_t frac = (uint32_t)(ppb < 0 ? 1000000000 + ppb : ppb);

		printk(" synced=%d ratio=%u.%09u drift=%s%u.%03uppm%s err=%dus corr=%dus driftcorr=%dus"
		       " refs=%u stale=%u steps=%u\n",
		       s.synced, ppb < 0 ? 0u : 1u, frac, ppb < 0 ? "-" : "+", mag / 1000u, mag % 1000u,
		       s.drift_valid ? "" : "(none)", s.last_error_us, s.last_correction_us,
		       s.drift_since_ref_us, s.refs - prev_refs, s.stale - prev_stale, s.steps);
		prev_refs = s.refs;
		prev_stale = s.stale;
	}
}

int gtime_init(void)
{
	timesync_init(&ts, TIMESYNC_REF_DELAY_US);
	k_thread_create(&pps_data, pps_stack, K_THREAD_STACK_SIZEOF(pps_stack), pps_thread,
			NULL, NULL, NULL, PRIO_PPS, 0, K_NO_WAIT);
	k_thread_name_set(&pps_data, "pps");
	return 0;
}
