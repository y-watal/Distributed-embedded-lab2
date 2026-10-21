/*
 * Lab 3 zone controller: shared dual-CAN layer, heartbeats, HB_MON with
 * degraded LEDs and zone failure, the N2 relay, test points, and the Lab 2
 * actuators and sensors for this zone (app.h: steer.c on N2, drive.c on N3).
 *
 * Threads (priority, lower = higher; all values in lab3_config.h):
 *   MCP2515 driver x2 (2)  SPI frame reads, toggles CAN_RX_A/B via canbus rx_cb
 *   relay      (3, N2)     forwards first copies to the other bus, RELAY_TX
 *   dispatch   (4)         dedup by (ID, seq), heartbeat ages, CTRL check
 *   steer      (5, N2)     CTRL -> servo angle
 *   drive      (5, N3)     CTRL -> brake / coast / PID target
 *   pid        (6, N3)     encoders -> motor duty every 10 ms
 *   hb_tx      (7)         HB every 20 ms on both buses
 *   hb_mon     (8)         every 5 ms: DEG_A/B, zone failure, ERR
 *   current    (10)        current sensors every 20 ms
 *   report     (12)        console summary once a second
 *
 * Console: USART2 / ST-LINK virtual COM port, 115200 baud.
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "app.h"
#include "canbus.h"
#include "hb.h"
#include "lab3_config.h"
#include "node_config.h"
#include "relay.h"
#include "report.h"
#include "tp.h"

int main(void)
{
	printk("Lab 3 zone %s: heartbeat 0x%03x every %d ms, relay %s\n", NODE_NAME,
	       ID_HB(NODE_SELF), HB_PERIOD_MS, RELAY_ENABLED ? "on" : "off");

	tp_init();
	if (TP_WALK_MS > 0) {
		tp_selftest(TP_WALK_MS);
	}
	/* Power up in the error state with both buses degraded until heartbeats arrive. */
	tp_set(TP_ERR, 1);
	tp_set(TP_DEG_A, 1);
	tp_set(TP_DEG_B, 1);

	canbus_set_handler(app_on_frame);
	if (canbus_init() < 0) {
		printk("no CAN controller came up\n");
	}
	relay_init();
	hb_init();
	report_init();

	/* Actuators safe, sensors calibrated (~0.75 s), then the zone's tasks. */
	int ret = app_init();

	if (ret < 0) {
		printk("app_init failed (%d): actuators stay in their safe state\n", ret);
	}
	return 0;
}
