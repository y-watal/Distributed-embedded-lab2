/*
 * Node-specific application, implemented by steer.c (N2) or drive.c (N3).
 * main.c and report.c are shared and call these.
 */
#ifndef APP_H
#define APP_H

#include <zephyr/drivers/can.h>

/* Puts actuators in their safe state, calibrates sensors, starts the tasks. */
int app_init(void);

/* Called by the dispatch thread with the first copy of each catalog frame. */
void app_on_frame(const struct can_frame *frame, int bus);

/* One or two lines for the once-a-second console summary. */
void app_report(void);

#endif /* APP_H */
