#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/pwm.h>

#include "servo.h"

#define SERVO_MIN_ANGLE_DEG 0
#define SERVO_MAX_ANGLE_DEG 180
#define SERVO_CENTER_DEG    90

#define SERVO_MIN_PULSE_US  500
#define SERVO_MAX_PULSE_US  2500

static const struct pwm_dt_spec servo_pwm =
    PWM_DT_SPEC_GET(DT_NODELABEL(steering_servo));

int servo_set_angle(int angle_deg)
{
    if (!device_is_ready(servo_pwm.dev)) {
        return -ENODEV;
    }

    if (angle_deg < SERVO_MIN_ANGLE_DEG ||
        angle_deg > SERVO_MAX_ANGLE_DEG) {
        return -EINVAL;
    }

    uint32_t pulse_us = SERVO_MIN_PULSE_US +
        (uint32_t)angle_deg *
        (SERVO_MAX_PULSE_US - SERVO_MIN_PULSE_US) /
        SERVO_MAX_ANGLE_DEG;

    return pwm_set_pulse_dt(&servo_pwm, PWM_USEC(pulse_us));
}

int servo_init(void)
{
    return servo_set_angle(SERVO_CENTER_DEG);
}