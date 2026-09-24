#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

int main(void)
{
    printk("Lab 2 firmware started\n");

    if (!gpio_is_ready_dt(&led)) {
        printk("LED GPIO is not ready\n");
        return 0;
    }

    int ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
    if (ret < 0) {
        printk("LED configuration failed: %d\n", ret);
        return 0;
    }

    while (1) {
        ret = gpio_pin_toggle_dt(&led);
        if (ret < 0) {
            printk("LED toggle failed: %d\n", ret);
            return 0;
        }

        k_msleep(500);
    }

    return 0;
}
