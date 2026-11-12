/*
 * Independent watchdog (IWDG, R4.5). Fed by HB_MON every WDT_CHECK_MS only if
 * every required thread checked in since the last feed; otherwise the board
 * resets after WDT_TIMEOUT_MS. A watchdog reset is reported at the next boot
 * (console, and FAULT code WATCHDOG_RESET in heartbeat byte 3).
 */
#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <zephyr/sys/util.h>

enum wdt_client {
	WDT_HB_TX = BIT(0),
	WDT_ACTUATOR = BIT(1), /* STEER on N2, DRIVE on N3 */
	WDT_ALL = WDT_HB_TX | WDT_ACTUATOR,
};

/* Call last in main: checks the reset cause, then starts the watchdog. */
int watchdog_init(void);

void watchdog_checkin(enum wdt_client who);

/* HB_MON, every MONITOR_PERIOD_MS. */
void watchdog_service(void);

#endif /* WATCHDOG_H */
