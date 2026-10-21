/*
 * BLINK_F (N2) / BLINK_R (N3): this zone's two blinker lamps (BLINK_LAMP_LEFT /
 * BLINK_LAMP_RIGHT in node_config.h).
 *
 * Follows the cockpit's BLINK frames: lamp on = its side's bit set and the
 * phase bit set, updated as soon as a frame arrives (both STM32s act on the
 * same frame, which is the front/rear sync). If the cockpit can't be heard
 * (lost, or no BLINK for BLINK_STALE_MS) or the zone is ISOLATED, the zone
 * flashes its own hazards at 1 Hz.
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
