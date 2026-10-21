#include "debug_pins.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/sys/printk.h>

#include <errno.h>
#include <stdbool.h>

/* PA6 / D12: Pi UART command received */
#define UART_COMMAND_PIN 6

/* PA7 / D11: motor duty cycle written (drive, coast, or brake) */
#define MOTOR_DUTY_PIN   7

static const struct device *const gpioa =
    DEVICE_DT_GET(DT_NODELABEL(gpioa));

static bool ready;

// Runs before main() and before any K_THREAD_DEFINE thread starts,
// so the pins are outputs before the first command or duty write
static int debug_pins_init(void)
{
    if (!device_is_ready(gpioa)) {
        printk("ERROR: debug pins: GPIOA not ready\n");
        return -ENODEV;
    }

    if (gpio_pin_configure(gpioa, UART_COMMAND_PIN, GPIO_OUTPUT_INACTIVE) < 0 ||
        gpio_pin_configure(gpioa, MOTOR_DUTY_PIN, GPIO_OUTPUT_INACTIVE) < 0) {
        printk("ERROR: debug pins: PA6/PA7 configure failed\n");
        return -EIO;
    }

    ready = true;
    return 0;
}

SYS_INIT(debug_pins_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

// Each toggle is a single BSRR write that only touches its own pin,
// so the two pins can be toggled from different threads safely
void debug_pin_uart_command_toggle(void)
{
    if (ready) {
        gpio_pin_toggle(gpioa, UART_COMMAND_PIN);
    }
}

void debug_pin_motor_duty_toggle(void)
{
    if (ready) {
        gpio_pin_toggle(gpioa, MOTOR_DUTY_PIN);
    }
}
