#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "timesync.h"

static int64_t iabs64(int64_t v)
{
	return v < 0 ? -v : v;
}

static void test_ref_conversion(void)
{
	struct raft_ref ref = {.cycle = 7};
	uint8_t frame[RAFT_REF_LEN];

	timesync_to_ref(12345678901ull, &ref);
	assert(ref.master_seconds == 12345 && ref.cycle == 7);
	assert(iabs64((int64_t)timesync_from_ref(&ref) - 12345678901ll) <= 8);
	timesync_to_ref(1999999ull, &ref); /* rounds into the next second */
	assert(ref.master_seconds == 2 && ref.fraction_65536 == 0);

	raft_ref_encode(&(struct raft_ref){.master_seconds = 1, .cycle = 0xBEEF}, frame);
	timesync_stamp_frame(frame, 5500000ull);
	raft_ref_decode(frame, &ref);
	assert(ref.master_seconds == 5 && ref.fraction_65536 == 32768 && ref.cycle == 0xBEEF);
}

/* Follower whose crystal runs 60 ppm fast and starts 3.7 s off, with jitter. */
static void test_converges_and_measures_drift(void)
{
	struct timesync t;
	uint64_t leader_offset = 1000000000ull; /* leader epoch differs from follower boot */
	int64_t worst_after_settle = 0;

	srand(1);
	timesync_init(&t, TIMESYNC_REF_DELAY_US);
	for (uint32_t k = 0; k < 500; k++) { /* 10 s of REF */
		uint64_t true_send = 20000ull * k + 5000;
		int jitter = rand() % 61 - 30; /* +-30 us receive jitter */
		uint64_t true_rx = true_send + TIMESYNC_REF_DELAY_US + (uint64_t)(int64_t)jitter;
		uint64_t local_rx = 3700000 + true_rx + true_rx * 60 / 1000000;
		struct raft_ref ref = {.cycle = (uint16_t)k};

		timesync_to_ref(leader_offset + true_send, &ref);
		assert(timesync_on_ref(&t, NODE_N1, &ref, local_rx));
		/* Duplicate copy from the other bus is ignored. */
		assert(!timesync_on_ref(&t, NODE_N1, &ref, local_rx + 300));

		/* Probe 10 ms later, between REFs. */
		uint64_t true_probe = true_rx + 10000;
		uint64_t local_probe = 3700000 + true_probe + true_probe * 60 / 1000000;
		int64_t err = (int64_t)(timesync_global(&t, local_probe) -
					(leader_offset + true_probe));

		if (k > 100 && iabs64(err) > worst_after_settle) {
			worst_after_settle = iabs64(err);
		}
	}
	assert(t.steps == 1 && t.stale == 500);
	assert(worst_after_settle < 60);
	assert(t.drift_valid && iabs64(t.drift_ppb + 60000) < 15000);

	/* Holdover: 500 ms without REF stays within drift-corrected bounds. */
	uint64_t true_probe = 20000ull * 499 + 5000 + 500000;
	uint64_t local_probe = 3700000 + true_probe + true_probe * 60 / 1000000;
	int64_t err = (int64_t)(timesync_global(&t, local_probe) - (leader_offset + true_probe));

	assert(iabs64(err) < 60 + 10);
	assert(timesync_is_synced(&t, false, local_probe));
	assert(!timesync_is_synced(&t, false, local_probe + TIMESYNC_HOLDOVER_US));
	assert(timesync_is_synced(&t, true, local_probe + TIMESYNC_HOLDOVER_US));
	printf("timesync: worst %lld us after settle, drift %d ppb\n",
	       (long long)worst_after_settle, t.drift_ppb);
}

/* A new leader whose clock is 300 us off: slewed in steps of at most 50 us. */
static void test_handover_slews(void)
{
	struct timesync t;
	struct raft_ref ref = {0};

	timesync_init(&t, 0);
	timesync_to_ref(1000000, &ref);
	assert(timesync_on_ref(&t, NODE_N1, &ref, 5000000));
	uint64_t before = timesync_global(&t, 5020000);

	ref.cycle = 3; /* new leader picks its own cycle numbers */
	timesync_to_ref(1020300, &ref);
	assert(timesync_on_ref(&t, NODE_N2, &ref, 5020000));
	assert(t.steps == 1 && t.last_error_us > 280 && t.last_correction_us == TIMESYNC_MAX_SLEW_US);
	assert(timesync_global(&t, 5020000) - before == TIMESYNC_MAX_SLEW_US);

	/* Frames from the old master's cycle space don't block the new one. */
	ref.cycle = 2;
	timesync_to_ref(1040300, &ref);
	assert(!timesync_on_ref(&t, NODE_N2, &ref, 5040000));

	/* An error past TIMESYNC_STEP_US is stepped. */
	ref.cycle = 4;
	timesync_to_ref(9000000, &ref);
	assert(timesync_on_ref(&t, NODE_N2, &ref, 5060000));
	assert(t.steps == 2 && timesync_global(&t, 5060000) == 9000000);
}

/* A leader that was never a follower uses its local clock as global time. */
static void test_unsynced_is_local(void)
{
	struct timesync t;

	timesync_init(&t, TIMESYNC_REF_DELAY_US);
	assert(timesync_global(&t, 123456789) == 123456789);
	assert(!timesync_is_synced(&t, false, 0) && timesync_is_synced(&t, true, 0));
	assert(timesync_drift_since_ref_us(&t, 99) == 0);
}

int main(void)
{
	test_ref_conversion();
	test_converges_and_measures_drift();
	test_handover_slews();
	test_unsynced_is_local();
	return 0;
}
