#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "led.h"

int main(void)
{
    if (led_init() < 0) {
        printk("LED initialization failed\n");
        return 0;
    }

    while (true) {
        /* All four blink together for 3 seconds */
        led_hazards_start();
        k_msleep(3000);

        /* Front right and back right blink for 3 seconds */
        led_right_start();
        k_msleep(3000);

        /* Front left and back left blink for 3 seconds */
        led_left_start();
        k_msleep(3000);

        /* Turn on each LED individually for 1 second */
        led_show_one(LED_FRONT_LEFT);
        k_msleep(1000);

        led_show_one(LED_FRONT_RIGHT);
        k_msleep(1000);

        led_show_one(LED_BACK_LEFT);
        k_msleep(1000);

        led_show_one(LED_BACK_RIGHT);
        k_msleep(1000);
    }
}