/*
 * TEST_BTN: the Nucleo's blue USER button B1 (PC13, the board's sw0). The GPIO
 * interrupt only gives a semaphore; this thread debounces (TEST_BTN_DEBOUNCE_MS
 * lockout after an accepted press, so the first edge acts at once) and hands
 * the press to the zone state machine. Single press = local fail, double
 * press = recover (zone.c).
 */
#include "button.h"

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "lab3_config.h"
#include "wcet.h"
#include "zone.h"

#define BUTTON_STACK_SIZE 1024

static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static struct gpio_callback button_cb;

K_SEM_DEFINE(button_sem, 0, 1);
K_THREAD_STACK_DEFINE(button_stack, BUTTON_STACK_SIZE);
static struct k_thread button_data;

static void button_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);
	k_sem_give(&button_sem);
}

static void button_thread(void *p1, void *p2, void *p3)
{
	uint32_t last_press = 0;
	bool first = true;
	struct wcet_job job = WCET_JOB(WCET_TEST_BTN);

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		wcet_end(&job);
		k_sem_take(&button_sem, K_FOREVER);
		wcet_begin(&job);

		uint32_t now = k_uptime_get_32();

		if (!first && now - last_press < TEST_BTN_DEBOUNCE_MS) {
			continue; /* contact bounce */
		}
		first = false;
		last_press = now;
		zone_test_button();
	}
}

int button_init(void)
{
	int ret;

	if (!gpio_is_ready_dt(&button)) {
		return -ENODEV;
	}
	ret = gpio_pin_configure_dt(&button, GPIO_INPUT);
	ret = ret ? ret : gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret < 0) {
		return ret;
	}
	gpio_init_callback(&button_cb, button_isr, BIT(button.pin));
	gpio_add_callback(button.port, &button_cb);

	k_thread_create(&button_data, button_stack, K_THREAD_STACK_SIZEOF(button_stack),
			button_thread, NULL, NULL, NULL, PRIO_TEST_BTN, 0, K_NO_WAIT);
	k_thread_name_set(&button_data, "test_btn");
	return 0;
}
