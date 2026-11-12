#include "watchdog.h"

#include <zephyr/device.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#include "lab3_config.h"
#include "zone.h"

static const struct device *const wdt = DEVICE_DT_GET(DT_NODELABEL(iwdg));
static int channel = -1;
static atomic_t checked_in;
static uint32_t last_check_ms;

void watchdog_checkin(enum wdt_client who)
{
	atomic_or(&checked_in, who);
}

void watchdog_service(void)
{
	uint32_t now = k_uptime_get_32();

	if (channel < 0 || now - last_check_ms < WDT_CHECK_MS) {
		return;
	}
	last_check_ms = now;
	if ((atomic_clear(&checked_in) & WDT_ALL) == WDT_ALL) {
		wdt_feed(wdt, channel);
	}
	/* else: a required thread is stuck; let the watchdog reset the board */
}

int watchdog_init(void)
{
	uint32_t cause = 0;

	if (hwinfo_get_reset_cause(&cause) == 0) {
		if (cause & RESET_WATCHDOG) {
			printk("*** last reset was the watchdog ***\n");
			zone_note_fault(FAULT_WATCHDOG_RESET);
		}
		hwinfo_clear_reset_cause();
	}

	if (!device_is_ready(wdt)) {
		printk("watchdog: device not ready\n");
		return -ENODEV;
	}

	struct wdt_timeout_cfg cfg = {
		.window = {.min = 0, .max = WDT_TIMEOUT_MS},
		.flags = WDT_FLAG_RESET_SOC,
	};
	int ch = wdt_install_timeout(wdt, &cfg);

	if (ch < 0) {
		printk("watchdog: install_timeout failed (%d)\n", ch);
		return ch;
	}
	/* Pause while a debugger halts the core, so breakpoints don't reset it. */
	int ret = wdt_setup(wdt, WDT_OPT_PAUSE_HALTED_BY_DBG);

	if (ret < 0) {
		printk("watchdog: setup failed (%d)\n", ret);
		return ret;
	}
	last_check_ms = k_uptime_get_32();
	atomic_set(&checked_in, WDT_ALL); /* first window counts from now */
	channel = ch;
	printk("watchdog: %d ms, fed every %d ms while HB_TX and the actuator task run\n",
	       WDT_TIMEOUT_MS, WDT_CHECK_MS);
	return 0;
}
