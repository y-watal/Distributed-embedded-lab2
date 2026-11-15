#include "timesync.h"

#include <string.h>

void timesync_init(struct timesync *t, int32_t delay_us)
{
	memset(t, 0, sizeof(*t));
	t->master = -1;
	t->delay_us = delay_us;
}

static int64_t drift_term_us(int32_t drift_ppb, int64_t dl_us)
{
	return dl_us * (int64_t)drift_ppb / 1000000000;
}

uint64_t timesync_global(const struct timesync *t, uint64_t local_us)
{
	int64_t dl = (int64_t)(local_us - t->base_local_us);

	return t->base_global_us + (uint64_t)(dl + drift_term_us(t->drift_ppb, dl));
}

int32_t timesync_drift_since_ref_us(const struct timesync *t, uint64_t local_us)
{
	if (!t->ever_synced) {
		return 0;
	}
	return (int32_t)drift_term_us(t->drift_ppb, (int64_t)(local_us - t->base_local_us));
}

void timesync_to_ref(uint64_t global_us, struct raft_ref *ref)
{
	uint32_t rem = (uint32_t)(global_us % 1000000u);
	uint32_t frac = (uint32_t)(((uint64_t)rem * 65536u + 500000u) / 1000000u);
	uint64_t seconds = global_us / 1000000u;

	if (frac > 0xFFFFu) { /* rounded up to the next second */
		frac = 0;
		seconds++;
	}
	ref->master_seconds = (uint32_t)seconds;
	ref->fraction_65536 = (uint16_t)frac;
}

uint64_t timesync_from_ref(const struct raft_ref *ref)
{
	return (uint64_t)ref->master_seconds * 1000000u +
	       ((uint64_t)ref->fraction_65536 * 1000000u + 32768u) / 65536u;
}

void timesync_stamp_frame(uint8_t frame[RAFT_REF_LEN], uint64_t global_us)
{
	struct raft_ref ref;

	raft_ref_decode(frame, &ref);
	timesync_to_ref(global_us, &ref);
	raft_ref_encode(&ref, frame);
}

static void update_drift(struct timesync *t, uint64_t leader_us, uint64_t local_us)
{
	int64_t df = (int64_t)(local_us - t->anchor_local_us);

	if (df < TIMESYNC_DRIFT_WINDOW_US) {
		return;
	}
	int64_t dl = (int64_t)(leader_us - t->anchor_leader_us);
	/* (L2 - L1) / (F2 - F1) - 1, in parts per billion */
	int64_t ppb = (dl - df) * 1000000000 / df;

	t->anchor_local_us = local_us;
	t->anchor_leader_us = leader_us;
	if (ppb > TIMESYNC_DRIFT_LIMIT_PPB || ppb < -TIMESYNC_DRIFT_LIMIT_PPB) {
		return; /* a gap or a master restart, not oscillator drift */
	}
	if (!t->drift_valid) {
		t->drift_ppb = (int32_t)ppb;
		t->drift_valid = true;
	} else {
		/* Exponential average over ~4 windows: RX jitter of tens of
		 * microseconds is tens of ppm in one 1 s window. */
		t->drift_ppb += (int32_t)((ppb - t->drift_ppb) / 4);
	}
}

bool timesync_on_ref(struct timesync *t, int master, const struct raft_ref *ref,
		     uint64_t local_us)
{
	bool new_master = master != t->master;
	bool long_gap = t->ever_synced &&
			local_us - t->last_ref_local_us > TIMESYNC_HOLDOVER_US;

	if (!new_master && !long_gap && t->ever_synced &&
	    (int16_t)(ref->cycle - t->last_cycle) <= 0) {
		t->stale++; /* the other bus's copy, a relayed copy, or an old frame */
		return false;
	}
	uint64_t leader_us = timesync_from_ref(ref);
	uint64_t measured = leader_us + (uint64_t)(int64_t)t->delay_us;
	uint64_t predicted = timesync_global(t, local_us);
	int64_t error = (int64_t)(measured - predicted);
	int64_t correction;
	bool stepped = !t->ever_synced || error > TIMESYNC_STEP_US ||
		       error < -TIMESYNC_STEP_US;

	if (stepped) {
		correction = error;
		t->steps++;
	} else {
		correction = error / 2;
		if (correction > TIMESYNC_MAX_SLEW_US) {
			correction = TIMESYNC_MAX_SLEW_US;
		} else if (correction < -TIMESYNC_MAX_SLEW_US) {
			correction = -TIMESYNC_MAX_SLEW_US;
		}
	}
	if (new_master || long_gap || stepped) {
		/* New baseline for the drift ratio: a different oscillator, or one
		 * whose time we just stepped onto. */
		t->anchor_local_us = local_us;
		t->anchor_leader_us = leader_us;
	} else {
		update_drift(t, leader_us, local_us);
	}
	t->base_local_us = local_us;
	t->base_global_us = predicted + (uint64_t)correction;
	t->last_error_us = (int32_t)error;
	t->last_correction_us = (int32_t)correction;
	t->last_ref_local_us = local_us;
	t->last_cycle = ref->cycle;
	t->master = (int8_t)master;
	t->ever_synced = true;
	t->refs++;
	return true;
}

bool timesync_is_synced(const struct timesync *t, bool is_leader, uint64_t local_us)
{
	if (is_leader) {
		return true;
	}
	return t->ever_synced && local_us - t->last_ref_local_us <= TIMESYNC_HOLDOVER_US;
}
