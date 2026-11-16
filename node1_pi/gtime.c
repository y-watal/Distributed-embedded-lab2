#define _POSIX_C_SOURCE 200809L
#include "gtime.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>

#include "raft_node.h"
#include "testpoints.h"
#include "timesync.h"

#ifndef PPS_GPIO
#define PPS_GPIO -1 /* no Pi pin assigned yet */
#endif
#define PPS_PULSE_US  100000
#define PPS_SPIN_US   300
#define PPS_QUEUE_LEN 4

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct timesync ts;

struct pps_snapshot {
	uint64_t global_us, local_us;
	int32_t drift_ppb, last_error_us, last_correction_us, drift_since_ref_us;
	int8_t master;
	bool leader, synced, drift_valid;
	uint32_t refs, steps, stale;
};
static struct pps_snapshot pps_ring[PPS_QUEUE_LEN];
static unsigned pps_head, pps_count;

uint64_t gtime_local_us(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

uint64_t global_gettime(void)
{
	pthread_mutex_lock(&lock);
	uint64_t g = timesync_global(&ts, gtime_local_us());

	pthread_mutex_unlock(&lock);
	return g;
}

void gtime_on_ref(uint32_t id, const uint8_t data[FRAME_DLC], uint64_t rx_ns)
{
	int leader, sender = catalog_sender(id);
	bool is_leader;
	struct raft_ref ref;

	raft_node_leader_snapshot(&leader, &is_leader);
	if (is_leader || sender < 0 || sender != leader) {
		return; /* only the current leader is our time master */
	}
	raft_ref_decode(data, &ref);
	pthread_mutex_lock(&lock);
	timesync_on_ref(&ts, sender, &ref, rx_ns / 1000u);
	pthread_mutex_unlock(&lock);
}

void gtime_stamp_ref(uint8_t data[FRAME_DLC])
{
	timesync_stamp_frame(data, global_gettime());
}

bool gtime_is_synced(void)
{
	int leader;
	bool is_leader;

	raft_node_leader_snapshot(&leader, &is_leader);
	pthread_mutex_lock(&lock);
	bool synced = timesync_is_synced(&ts, is_leader, gtime_local_us());

	pthread_mutex_unlock(&lock);
	return synced;
}

static void sleep_us(uint64_t us)
{
	struct timespec t = {.tv_sec = (time_t)(us / 1000000u),
			     .tv_nsec = (long)(us % 1000000u) * 1000L};

	while (nanosleep(&t, &t) != 0 && errno == EINTR) {
	}
}

static void *pps_thread(void *arg)
{
	(void)arg;
	for (;;) {
		uint64_t g = global_gettime();
		uint64_t edge = (g / 1000000u + 1u) * 1000000u;

		if (edge - g > PPS_SPIN_US) {
			sleep_us(edge - g - PPS_SPIN_US);
		}
		uint64_t spin_start = gtime_local_us();

		while ((g = global_gettime()) < edge &&
		       gtime_local_us() - spin_start < 4 * PPS_SPIN_US) {
		}
		if (g < edge || g - edge > 500000u) {
			continue; /* clock stepped: no stale pulse */
		}
		if (PPS_GPIO >= 0) {
			tp_set(PPS_GPIO, 1);
		}

		int leader;
		bool is_leader;

		raft_node_leader_snapshot(&leader, &is_leader);
		pthread_mutex_lock(&lock);
		uint64_t local = gtime_local_us();
		struct pps_snapshot s = {
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
		pps_ring[(pps_head + pps_count) % PPS_QUEUE_LEN] = s;
		if (pps_count < PPS_QUEUE_LEN) {
			pps_count++;
		} else {
			pps_head = (pps_head + 1) % PPS_QUEUE_LEN; /* drop the oldest */
		}
		pthread_mutex_unlock(&lock);

		sleep_us(PPS_PULSE_US);
		if (PPS_GPIO >= 0) {
			tp_set(PPS_GPIO, 0);
		}
	}
	return NULL;
}

void gtime_report(void)
{
	static uint32_t prev_refs, prev_stale;

	for (;;) {
		struct pps_snapshot s;

		pthread_mutex_lock(&lock);
		if (!pps_count) {
			pthread_mutex_unlock(&lock);
			return;
		}
		s = pps_ring[pps_head];
		pps_head = (pps_head + 1) % PPS_QUEUE_LEN;
		pps_count--;
		pthread_mutex_unlock(&lock);

		uint32_t mag = (uint32_t)(s.drift_ppb < 0 ? -s.drift_ppb : s.drift_ppb);
		char master[8];

		if (s.leader) {
			snprintf(master, sizeof(master), "self");
		} else if (s.master >= 0) {
			snprintf(master, sizeof(master), "N%d", s.master + 1);
		} else {
			snprintf(master, sizeof(master), "?");
		}
		printf("  SYNC: global=%llu.%06llu local=%llu.%06llu master=%s synced=%d"
		       " drift=%s%u.%03uppm%s err=%dus corr=%dus driftcorr=%dus"
		       " refs=%u stale=%u steps=%u\n",
		       (unsigned long long)(s.global_us / 1000000u),
		       (unsigned long long)(s.global_us % 1000000u),
		       (unsigned long long)(s.local_us / 1000000u),
		       (unsigned long long)(s.local_us % 1000000u), master, s.synced,
		       s.drift_ppb < 0 ? "-" : "+", mag / 1000u, mag % 1000u,
		       s.drift_valid ? "" : "(none)", s.last_error_us, s.last_correction_us,
		       s.drift_since_ref_us, s.refs - prev_refs, s.stale - prev_stale, s.steps);
		prev_refs = s.refs;
		prev_stale = s.stale;
	}
}

int gtime_init(void)
{
	pthread_t t;

	timesync_init(&ts, TIMESYNC_REF_DELAY_US);
	if (pthread_create(&t, NULL, pps_thread, NULL) != 0) {
		return -1;
	}
	pthread_detach(t);
	return 0;
}
