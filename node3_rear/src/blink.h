/*
 * BLINK_F (N2) / BLINK_R (N3): this zone's two blinker lamps (BLINK_LAMP_LEFT /
 * BLINK_LAMP_RIGHT in node_config.h).
 *
 * The cockpit's BLINK frames choose the mode (off, left, right, hazard). The
 * lamp phase comes from global_gettime() (shared/blink_phase.h), so front and
 * rear switch at the same global instant: turn signals 1 Hz, hazards 2 Hz.
 * If N1's heartbeat is lost while this zone still hears the bus (cockpit
 * down: failure mode) it flashes hazards at 4 Hz. If the zone is isolated
 * (ISOLATED, or both buses degraded) or no BLINK arrives for BLINK_STALE_MS
 * (2.6 s), it flashes hazards at 2 Hz. Phases stay
 * global-time aligned in every case.
 */
#ifndef BLINK_H
#define BLINK_H

#include <zephyr/drivers/can.h>

int blink_init(void);

/* Dispatch thread, first copy of each BLINK frame. */
void blink_on_frame(const struct can_frame *frame);

/* For the report: "LEFT on", "HAZARD (local) off", ... */
const char *blink_describe(void);

#endif /* BLINK_H */
