/*
 * WCET measurement: the CPU time a thread spends on one job.
 *
 * The clock is Zephyr's per-thread usage counter (CONFIG_SCHED_THREAD_USAGE).
 * It only advances while the thread is switched in, so time spent preempted
 * by another thread, or blocked on a mutex / semaphore / SPI / ADC, is not
 * counted. ISR time that lands inside a job is counted (errs on the safe side).
 * Resolution is one CPU cycle (84 MHz, k_cycle_get_32()).
 *
 * In a thread loop:
 *
 *     struct wcet_job job = WCET_JOB(WCET_DRIVE);
 *
 *     for (;;) {
 *             wcet_end(&job);         // closes the previous job, if one is open
 *             k_sem_take(...);        // wait for the next release
 *             wcet_begin(&job);
 *             ...                     // `continue` is fine: the next wcet_end closes it
 *     }
 *
 * The REPORT thread prints max / mean / min per task every WCET_REPORT_EVERY
 * summaries. WCET_MEASURE 0 (lab3_config.h) compiles every probe away.
 */
#ifndef WCET_H
#define WCET_H

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/kernel.h>

#include "lab3_config.h"

enum wcet_slot {
	WCET_DISPATCH,      /* one frame other than CTRL: dedup, HB times, route */
	WCET_DISPATCH_CTRL, /* one CTRL: CRC/range, store, wake STEER/DRIVE (reject: + zone_update, FAULT) */
	WCET_RELAY,         /* N2: forward one frame */
	WCET_STEER,         /* N2 */
	WCET_DRIVE,         /* N3 */
	WCET_PID,           /* N3 */
	WCET_BLINK,         /* BLINK_F / BLINK_R */
	WCET_TEST_BTN,      /* incl. zone_update and the FAULT send */
	WCET_HB_TX,
	WCET_HB_MON,        /* incl. zone_update, FAULT send, watchdog */
	WCET_FORCE_TX,      /* N2 */
	WCET_CURRENT,
	WCET_REPORT,
	WCET_NUM_SLOTS
};

struct wcet_job {
	uint64_t t0;   /* thread's execution cycles at wcet_begin() */
	uint8_t slot;  /* may be changed while the job runs (DISPATCH vs DISPATCH_CTRL) */
	bool open;
};

#define WCET_JOB(s) {.t0 = 0, .slot = (s), .open = false}

#if WCET_MEASURE

#if !defined(CONFIG_SCHED_THREAD_USAGE)
#error "WCET_MEASURE needs CONFIG_THREAD_RUNTIME_STATS=y and CONFIG_SCHED_THREAD_USAGE=y in prj.conf"
#endif

void wcet_begin(struct wcet_job *job);
void wcet_end(struct wcet_job *job);

/* Called from rx_cb, i.e. in the MCP2515 driver thread of that bus. */
void wcet_driver_seen(int bus);

/* Calibrates the probe overhead. Call once from main(). */
void wcet_init(void);

/* Prints the table (REPORT thread). */
void wcet_report(void);

#else

static inline void wcet_begin(struct wcet_job *job) { ARG_UNUSED(job); }
static inline void wcet_end(struct wcet_job *job) { ARG_UNUSED(job); }
static inline void wcet_driver_seen(int bus) { ARG_UNUSED(bus); }
static inline void wcet_init(void) {}
static inline void wcet_report(void) {}

#endif /* WCET_MEASURE */

#endif /* WCET_H */
