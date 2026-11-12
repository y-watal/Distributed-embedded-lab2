#include "wcet.h"

#if WCET_MEASURE

#include <string.h>

#include <zephyr/sys/printk.h>
#include <zephyr/sys/time_units.h>

#include "canbus.h"

/* Padded so the columns line up without printf field widths. */
static const char *const slot_name[WCET_NUM_SLOTS] = {
	[WCET_DISPATCH] = "DISPATCH     ",
	[WCET_DISPATCH_CTRL] = "DISPATCH CTRL",
	[WCET_RELAY] = "RELAY        ",
	[WCET_STEER] = "STEER        ",
	[WCET_DRIVE] = "DRIVE        ",
	[WCET_PID] = "PID          ",
	[WCET_BLINK] = "BLINK        ",
	[WCET_TEST_BTN] = "TEST_BTN     ",
	[WCET_HB_TX] = "HB_TX        ",
	[WCET_HB_MON] = "HB_MON       ",
	[WCET_FORCE_TX] = "FORCE_TX     ",
	[WCET_CURRENT] = "CURRENT      ",
	[WCET_REPORT] = "REPORT       ",
};

struct wcet_stat {
	uint32_t n, min, max; /* cycles */
	uint64_t sum;
};

static struct wcet_stat stats[WCET_NUM_SLOTS];
static struct wcet_stat snapshot[WCET_NUM_SLOTS]; /* REPORT's copy, off its stack */
static struct k_spinlock lock;
static uint32_t probe_overhead;                  /* cycles, included in every job */
static k_tid_t driver_tid[NUM_BUSES];

static uint64_t thread_cycles(void)
{
	k_thread_runtime_stats_t s;

	k_thread_runtime_stats_get(k_current_get(), &s);
	return s.execution_cycles;
}

void wcet_begin(struct wcet_job *job)
{
	job->t0 = thread_cycles();
	job->open = true;
}

void wcet_end(struct wcet_job *job)
{
	if (!job->open) {
		return;
	}
	uint32_t c = (uint32_t)(thread_cycles() - job->t0);

	job->open = false;

	k_spinlock_key_t key = k_spin_lock(&lock);
	struct wcet_stat *st = &stats[job->slot];

	if (st->n == 0 || c < st->min) {
		st->min = c;
	}
	if (c > st->max) {
		st->max = c;
	}
	st->sum += c;
	st->n++;
	k_spin_unlock(&lock, key);
}

void wcet_driver_seen(int bus)
{
	if (bus >= 0 && bus < NUM_BUSES && driver_tid[bus] == NULL) {
		driver_tid[bus] = k_current_get();
	}
}

void wcet_init(void)
{
	/* An empty begin/end: what the probe itself adds to every job. */
	uint32_t best = UINT32_MAX;

	for (int i = 0; i < 32; i++) {
		uint64_t t0 = thread_cycles();
		uint32_t c = (uint32_t)(thread_cycles() - t0);

		best = MIN(best, c);
	}
	probe_overhead = best;
}

/* Prints cycles as microseconds with one decimal, rounded up. */
static void put_us(const char *label, uint64_t cycles)
{
	uint64_t ns = k_cyc_to_ns_ceil64(cycles);
	uint32_t tenths = (uint32_t)((ns + 99) / 100);

	printk("  %s %u.%u", label, tenths / 10, tenths % 10);
}

void wcet_report(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	memcpy(snapshot, stats, sizeof(snapshot));
	k_spin_unlock(&lock, key);

	printk("  WCET since boot, us of the thread's own CPU time per job (preemption and\n"
	       "  blocking excluded, ISRs inside the job included,");
	put_us("probe overhead included:", probe_overhead);
	printk(")\n");
	for (int i = 0; i < WCET_NUM_SLOTS; i++) {
		struct wcet_stat *st = &snapshot[i];

		if (st->n == 0) {
			continue;
		}
		printk("    %s", slot_name[i]);
		put_us("max", st->max);
		put_us("mean", st->sum / st->n);
		put_us("min", st->min);
		printk("  jobs %u\n", st->n);
	}

#ifdef CONFIG_SCHED_THREAD_USAGE_ANALYSIS
	/*
	 * Zephyr's MCP2515 driver threads are cooperative, so each run from wake-up
	 * to blocking again is one uninterrupted stretch: an RX or TX-done job (or
	 * two events handled in one pass when both RX buffers were full). The
	 * longest such stretch is the driver job's WCET.
	 */
	for (int bus = 0; bus < NUM_BUSES; bus++) {
		k_thread_runtime_stats_t s;

		if (driver_tid[bus] == NULL) {
			continue;
		}
		k_thread_runtime_stats_get(driver_tid[bus], &s);
		printk("    MCP2515 %c     ", 'A' + bus);
		put_us("longest run", s.peak_cycles);
		put_us("mean run", s.average_cycles);
		printk("\n");
	}
#endif
}

#endif /* WCET_MEASURE */
