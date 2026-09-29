#include "blinkers.h"
#include "current_sensor.h"
#include "current_sensing.h"
#include "encoder.h"
#include "led.h"
#include "motor.h"
#include "motor_control.h"
#include "safety.h"
#include "servo.h"
#include "steering.h"
#include "status.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

int main(void)
{
    printk("Lab 2 firmware starting in ERROR state\n");

    int ret = motor_init();
    if (ret < 0) {
        printk("ERROR: motor initialization failed: %d\n", ret);
        return 0;
    }

    ret = led_init();
    if (ret < 0) {
        printk("ERROR: LED initialization failed: %d\n", ret);
        return 0;
    }

    led_hazards_start();

    /* Calibrate before enabling servo pulses or starting actuator tasks.
     * Sensors must be powered, with no current through their load paths.
     */
    ret = current_sensor_init();
    if (ret < 0) {
        printk("ERROR: current sensor initialization failed: %d\n", ret);
        return 0;
    }

    ret = servo_init();
    if (ret < 0) {
        printk("ERROR: servo initialization failed: %d\n", ret);
        return 0;
    }

    ret = encoder_init();
    if (ret < 0) {
        printk("ERROR: encoder initialization failed: %d\n", ret);
        return 0;
    }

    printk("state=ERROR waiting for valid Pi command\n");

    safety_start();
    motor_control_start();
    steering_start();
    blinkers_start();
    current_sensing_start();
    status_start();

    return 0;
}
