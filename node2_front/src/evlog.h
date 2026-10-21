/*
 * Event log: one timestamped line per state change (zone transitions, local
 * hazards, blink mode changes), so a short glitch shows up with its cause
 * instead of being averaged away by the 1 s summary.
 *
 * evlog() only formats into a queue and never blocks; a low-priority thread
 * (below REPORT, so it never splits a summary line) does the printk. If the
 * queue is full the line is dropped and counted.
 */
#ifndef EVLOG_H
#define EVLOG_H

#include <zephyr/sys/atomic.h>

void evlog(const char *fmt, ...);

extern atomic_t evlog_dropped;

#endif /* EVLOG_H */
