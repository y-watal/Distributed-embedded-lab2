#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);

int main(void)
{
    if (!gpio_is_ready_dt(&led) || !gpio_is_ready_dt(&button)) {
        printk("LED or button GPIO is not ready\n");
        return 0;
    }

    int ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
    if (ret < 0) {
        printk("LED configuration failed: %d\n", ret);
        return 0;
    }

    ret = gpio_pin_configure_dt(&button, GPIO_INPUT);
    if (ret < 0) {
        printk("Button configuration failed: %d\n", ret);
        return 0;
    }

    printk("Hold B1 to blink LD2\n");

    bool blinking = false;
    bool led_on = false;
    int64_t next_toggle = 0;

    while (1) {
        int pressed = gpio_pin_get_dt(&button);
        if (pressed < 0) {
            printk("Button read failed: %d\n", pressed);
            return 0;
        }

        int64_t now = k_uptime_get();

        if (pressed) {
            if (!blinking) {
                blinking = true;
                led_on = true;
                next_toggle = now + 500;
            } else if (now >= next_toggle) {
                led_on = !led_on;
                next_toggle = now + 500;
            }
        } else {
            blinking = false;
            led_on = false;
        }

        ret = gpio_pin_set_dt(&led, led_on);
        if (ret < 0) {
            printk("LED update failed: %d\n", ret);
            return 0;
        }

        k_msleep(10);
    }

    return 0;
}
