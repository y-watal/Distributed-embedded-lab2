/*
 * Lab 3 test points (GPIO outputs the TAs clip onto).
 *
 * The same file is used on N2 and N3. Each node's app.overlay lists the
 * points it has as <name>-gpios properties of the zephyr,user node; a point
 * the node doesn't have is a no-op, so shared code can call any of them.
 * Pin and breadboard-row assignments: docs/lab3_pin_map.md. Rows that are
 * actuator signals themselves (PWM_OUT, DIR_A) are wired to those pins and
 * driven by motor.c, not by this module.
 */
#ifndef TP_H
#define TP_H

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/drivers/gpio.h>

enum tp_id {
	TP_CAN_RX_A,  /* dispatch saw a frame from controller A (after the SPI read) */
	TP_CAN_RX_B,
	TP_CAN_TX_A,  /* call that queues a frame on controller A */
	TP_CAN_TX_B,
	TP_DEG_A,     /* LED: bus A degraded at this node (R4.7) */
	TP_DEG_B,
	TP_ERR,       /* high while the zone is in the error state */
	TP_FL,        /* blinker LEDs */
	TP_FR,
	TP_RL,
	TP_RR,
	TP_RELAY_TX,  /* relay queued a forwarded frame (N2) */
	TP_PWM_SET,   /* motor duty written (N3) */
	TP_DIR_A,     /* unused: on N3 the DIR_A row is the H-bridge IN1 pin itself */
	TP_SRV,       /* servo duty set (N2): once per CTRL STEER acts on, and on re-centering */
	TP_COUNT
};

extern const struct gpio_dt_spec tp_pins[TP_COUNT];

/* Configures every point this node has as an output, driven low. */
int tp_init(void);

/* Drives each point high for ms_per_point, one at a time, printing its name. */
void tp_selftest(int ms_per_point);

/* ISR-safe and cheap; no-ops for points this node doesn't have. */
static inline void tp_toggle(enum tp_id id)
{
	if (tp_pins[id].port != NULL) {
		gpio_pin_toggle_dt(&tp_pins[id]);
	}
}

static inline void tp_set(enum tp_id id, int value)
{
	if (tp_pins[id].port != NULL) {
		gpio_pin_set_dt(&tp_pins[id], value);
	}
}

#endif /* TP_H */
