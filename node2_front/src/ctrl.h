/*
 * Latest valid CTRL command, shared by the dispatch thread (writer) and the
 * node's actuator task (DRIVE on N3, STEER on N2). ctrl_on_frame() runs in
 * dispatch; a valid frame wakes the actuator task right away (brake path).
 */
#ifndef CTRL_H
#define CTRL_H

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/drivers/can.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include "catalog.h"

void ctrl_on_frame(const struct can_frame *frame);

/* Copies the latest valid command and its age in ms; false if none yet. */
bool ctrl_get(struct ctrl_cmd *cmd, uint32_t *age_ms);

/* True if a valid command arrived less than CTRL_TIMEOUT_MS ago. */
bool ctrl_fresh(void);

/* Blocks until a new valid command arrives or timeout; true if one arrived. */
bool ctrl_wait(k_timeout_t timeout);

/* Wakes the actuator task early (e.g. the zone just left NORMAL). */
void ctrl_kick(void);

struct ctrl_stats {
	atomic_t ok, malformed, out_of_range;
	atomic_t last_fault; /* enum fault_code of the last rejected frame */
};
extern struct ctrl_stats ctrl_stats;

#endif /* CTRL_H */
