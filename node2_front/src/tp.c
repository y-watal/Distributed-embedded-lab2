#include "tp.h"

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define ZU DT_PATH(zephyr_user)

#define TP_SPEC(prop) \
	COND_CODE_1(DT_NODE_HAS_PROP(ZU, prop), (GPIO_DT_SPEC_GET(ZU, prop)), ({0}))

const struct gpio_dt_spec tp_pins[TP_COUNT] = {
	[TP_CAN_RX_A] = TP_SPEC(can_rx_a_gpios),
	[TP_CAN_RX_B] = TP_SPEC(can_rx_b_gpios),
	[TP_CAN_TX_A] = TP_SPEC(can_tx_a_gpios),
	[TP_CAN_TX_B] = TP_SPEC(can_tx_b_gpios),
	[TP_DEG_A] = TP_SPEC(deg_a_gpios),
	[TP_DEG_B] = TP_SPEC(deg_b_gpios),
	[TP_ERR] = TP_SPEC(err_gpios),
	[TP_FL] = TP_SPEC(fl_gpios),
	[TP_FR] = TP_SPEC(fr_gpios),
	[TP_RL] = TP_SPEC(rl_gpios),
	[TP_RR] = TP_SPEC(rr_gpios),
	[TP_RELAY_TX] = TP_SPEC(relay_tx_gpios),
	[TP_PWM_SET] = TP_SPEC(pwm_set_gpios),
	[TP_DIR_A] = TP_SPEC(dir_a_gpios),
	[TP_SRV] = TP_SPEC(srv_gpios),
};

static const char *const tp_names[TP_COUNT] = {
	[TP_CAN_RX_A] = "CAN_RX_A", [TP_CAN_RX_B] = "CAN_RX_B", [TP_CAN_TX_A] = "CAN_TX_A",
	[TP_CAN_TX_B] = "CAN_TX_B", [TP_DEG_A] = "DEG_A",       [TP_DEG_B] = "DEG_B",
	[TP_ERR] = "ERR",           [TP_FL] = "FL",             [TP_FR] = "FR",
	[TP_RL] = "RL",             [TP_RR] = "RR",             [TP_RELAY_TX] = "RELAY_TX",
	[TP_PWM_SET] = "PWM_SET",   [TP_DIR_A] = "DIR_A",       [TP_SRV] = "SRV",
};

int tp_init(void)
{
	int ret = 0;

	for (int i = 0; i < TP_COUNT; i++) {
		if (tp_pins[i].port == NULL) {
			continue;
		}
		if (!gpio_is_ready_dt(&tp_pins[i]) ||
		    gpio_pin_configure_dt(&tp_pins[i], GPIO_OUTPUT_INACTIVE) < 0) {
			printk("test point %s: configure failed\n", tp_names[i]);
			ret = -EIO;
		}
	}
	return ret;
}

void tp_selftest(int ms_per_point)
{
	printk("Test point walk, %d ms each:", ms_per_point);
	for (int i = 0; i < TP_COUNT; i++) {
		if (tp_pins[i].port == NULL) {
			continue;
		}
		printk(" %s", tp_names[i]);
		gpio_pin_set_dt(&tp_pins[i], 1);
		k_msleep(ms_per_point);
		gpio_pin_set_dt(&tp_pins[i], 0);
	}
	printk("\n");
}
