#include "current_sensing.h"
#include "current_sensor.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define CURRENT_PRINT_PERIOD_MS 250
#define CURRENT_SAMPLE_PERIOD_MS 20
#define CURRENT_SENSING_STACK_SIZE 1024
#define CURRENT_SENSING_PRIORITY 6

K_THREAD_STACK_DEFINE(current_sensing_stack, CURRENT_SENSING_STACK_SIZE);
static struct k_thread current_sensing_thread_data;
K_MUTEX_DEFINE(current_readings_mutex);
static struct current_sensor_readings latest_readings;
static bool have_readings;

bool current_sensing_get_latest(struct current_sensor_readings *readings)
{
    if (readings == NULL) {
        return false;
    }

    k_mutex_lock(&current_readings_mutex, K_FOREVER);
    bool available = have_readings;
    if (available) {
        *readings = latest_readings;
    }
    k_mutex_unlock(&current_readings_mutex);
    return available;
}

static void current_sensing_thread(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    int64_t last_print_ms = 0;

    while (1) {
        struct current_sensor_readings readings;
        int ret = current_sensor_read(&readings);

        if (ret == 0) {
            k_mutex_lock(&current_readings_mutex, K_FOREVER);
            latest_readings = readings;
            have_readings = true;
            k_mutex_unlock(&current_readings_mutex);

            int64_t now_ms = k_uptime_get();
            if (now_ms - last_print_ms >= CURRENT_PRINT_PERIOD_MS) {
                last_print_ms = now_ms;
                printk("Current: left=%d mA | right=%d mA | servo=%d mA\n",
                       (int)readings.left_ma, (int)readings.right_ma,
                       (int)readings.servo_ma);
            }
        } else {
            printk("ERROR: current sensor read failed: %d\n", ret);
        }

        k_sleep(K_MSEC(CURRENT_SAMPLE_PERIOD_MS));
    }
}

void current_sensing_start(void)
{
    k_thread_create(&current_sensing_thread_data, current_sensing_stack,
                    K_THREAD_STACK_SIZEOF(current_sensing_stack),
                    current_sensing_thread, NULL, NULL, NULL,
                    CURRENT_SENSING_PRIORITY, 0, K_NO_WAIT);
}
