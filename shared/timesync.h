/*
 * Lab 5 Part 6: global time from the leader's REF.
 *
 * Portable (no Zephyr or POSIX calls) so the Pi, both STM32s and the host
 * tests share it. Every time is in microseconds. "Local" is the node's own
 * free-running clock; "global" is the cluster time the leader stamps into REF.
 *
 * Model between REFs (global_gettime()):
 *     global(local) = base_global + dl + dl * drift_ppb / 1e9,  dl = local - base_local
 *
 * On each accepted REF carrying leader time L, received at local time F:
 *     measured  = L + delay_us                (handout 6.1: follower = L1 + network delay)
 *     error     = measured - global(F)        (what the clock is off by right now)
 *     |error| > TIMESYNC_STEP_US  -> step:  base = (F, measured)
 *     otherwise                  -> slew:  base = (F, global(F) + clamp(error / 2))
 * The drift ratio is the handout's (L2 - L1) / (F2 - F1), measured over at
 * least TIMESYNC_DRIFT_WINDOW_US of REFs from one master and smoothed.
 *
 * A leader is its own time master: it keeps running the same model, so a
 * follower that is elected continues the agreed time with no step. A node
 * that has never synchronized uses its local clock as global (epoch = boot).
 */
#ifndef SHARED_TIMESYNC_H
#define SHARED_TIMESYNC_H

#include <stdbool.h>
#include <stdint.h>

#include "raft_wire.h"

/*
 * REF stamp (taken just before the frame is handed to the CAN controller) to
 * receive timestamp: controller load (~50 us), one 8-byte frame at 500 kbit/s
 * with stuffing (~240 us), receiver interrupt and SPI read (~60 us). Leader
 * side error is common to every follower, so it does not move FL against RL.
 */
#ifndef TIMESYNC_REF_DELAY_US
#define TIMESYNC_REF_DELAY_US     350
#endif
#define TIMESYNC_STEP_US          2000    /* larger error: set the clock outright */
#define TIMESYNC_MAX_SLEW_US      50      /* most one REF may move the clock */
#define TIMESYNC_DRIFT_WINDOW_US  1000000 /* baseline for (L2 - L1) / (F2 - F1) */
#define TIMESYNC_DRIFT_LIMIT_PPB  500000   /* reject a ratio worse than 500 ppm */
#define TIMESYNC_HOLDOVER_US      1000000 /* SYNC stays on this long without REF */

struct timesync {
	bool ever_synced;      /* a REF has set the model at least once */
	int8_t master;         /* node whose REFs feed the model; -1 none yet */
	uint16_t last_cycle;
	uint64_t base_local_us;
	uint64_t base_global_us;
	int32_t drift_ppb;     /* global runs (1 + drift_ppb / 1e9) times local */
	bool drift_valid;
	uint64_t anchor_local_us;  /* F1 for the drift ratio */
	uint64_t anchor_leader_us; /* L1 for the drift ratio */
	uint64_t last_ref_local_us;
	int32_t last_error_us;      /* measured - predicted at the last REF */
	int32_t last_correction_us; /* how far that REF moved the clock */
	int32_t delay_us;
	uint32_t refs, steps, stale; /* accepted, stepped, rejected as old/repeated */
};

void timesync_init(struct timesync *t, int32_t delay_us);

/* global_gettime() for a given local time. */
uint64_t timesync_global(const struct timesync *t, uint64_t local_us);

/*
 * A REF from `master` (the sender in its CAN ID) received at local_us. The
 * caller passes only REFs from the node it believes is leader. Returns true
 * if it was used; a repeated copy (other bus, relay) returns false.
 */
bool timesync_on_ref(struct timesync *t, int master, const struct raft_ref *ref,
		     uint64_t local_us);

/* Leader, or a follower that heard its master within TIMESYNC_HOLDOVER_US. */
bool timesync_is_synced(const struct timesync *t, bool is_leader, uint64_t local_us);

/* Drift correction applied since the last REF (the model's rate term). */
int32_t timesync_drift_since_ref_us(const struct timesync *t, uint64_t local_us);

/* Global microseconds <-> REF seconds and 1/65536 fraction. */
void timesync_to_ref(uint64_t global_us, struct raft_ref *ref);
uint64_t timesync_from_ref(const struct raft_ref *ref);

/* Rewrite bytes 0-5 of an encoded REF with a new global time; keeps the cycle. */
void timesync_stamp_frame(uint8_t frame[RAFT_REF_LEN], uint64_t global_us);

#endif /* SHARED_TIMESYNC_H */
