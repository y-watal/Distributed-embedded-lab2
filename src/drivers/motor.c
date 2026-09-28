#include "motor.h"

#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <stdbool.h>

enum motor_pin {
    ENA,
    ENB,
    IN1,
    IN2,
    IN3,
    IN4
};

// Initialize enables first so both bridges start disabled
static const struct gpio_dt_spec motor_pins[] = {
    { .port = DEVICE_DT_GET(DT_NODELABEL(gpioa)), .pin = 0, .dt_flags = 0 },
    { .port = DEVICE_DT_GET(DT_NODELABEL(gpioa)), .pin = 1, .dt_flags = 0 },
    { .port = DEVICE_DT_GET(DT_NODELABEL(gpiob)), .pin = 12, .dt_flags = 0 },
    { .port = DEVICE_DT_GET(DT_NODELABEL(gpiob)), .pin = 13, .dt_flags = 0 },
    { .port = DEVICE_DT_GET(DT_NODELABEL(gpiob)), .pin = 14, .dt_flags = 0 },
    { .port = DEVICE_DT_GET(DT_NODELABEL(gpiob)), .pin = 15, .dt_flags = 0 }
};

static bool initialized;

/*
 * Configure the H-bridge control pins and select a fixed motor direction
 *
 * Both enables remain low throughout initialization
 * Returns zero on success or a negative error code
 * Call once during startup before using motor_set_enabled
 */
int motor_init(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(motor_pins); i++) {
        if (!gpio_is_ready_dt(&motor_pins[i])) {
            return -ENODEV;
        }

        int ret = gpio_pin_configure_dt(&motor_pins[i], GPIO_OUTPUT_INACTIVE);
        if (ret < 0) {
            return ret;
        }
    }

    // Select one direction for both motors
    // IN2 and IN4 remain low from initialization
    int ret = gpio_pin_set_dt(&motor_pins[IN1], 1);
    if (ret < 0) {
        return ret;
    }

    ret = gpio_pin_set_dt(&motor_pins[IN3], 1);
    if (ret < 0) {
        return ret;
    }

    initialized = true;
    return 0;
}

/*
 * Enable both motors at full drive or disable them to coast
 *
 * A constant high on ENA and ENB gives 100 percent duty cycle
 * A constant low disables the outputs without dynamic braking
 * Attempt to disable both bridges if either GPIO operation fails
 *
 * This bring-up driver is intended to have one controlling thread
 */
int motor_set_enabled(bool enabled)
{
    if (!initialized) {
        return -ENODEV;
    }

    int left_ret = gpio_pin_set_dt(&motor_pins[ENA], enabled);
    int right_ret = gpio_pin_set_dt(&motor_pins[ENB], enabled);

    if (left_ret < 0 || right_ret < 0) {
        gpio_pin_set_dt(&motor_pins[ENA], 0);
        gpio_pin_set_dt(&motor_pins[ENB], 0);
        return left_ret < 0 ? left_ret : right_ret;
    }

    return 0;
}