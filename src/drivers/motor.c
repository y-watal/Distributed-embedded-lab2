#include "motor.h"

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

enum motor_pin {
    IN1,
    IN2,
    IN3,
    IN4
};

enum motor_mode {
    MOTOR_MODE_UNKNOWN,
    MOTOR_MODE_COAST,
    MOTOR_MODE_BRAKE,
    MOTOR_MODE_DRIVE
};

static const struct gpio_dt_spec input_pins[] = {
    { .port = DEVICE_DT_GET(DT_NODELABEL(gpiob)), .pin = 12, .dt_flags = 0 },
    { .port = DEVICE_DT_GET(DT_NODELABEL(gpiob)), .pin = 13, .dt_flags = 0 },
    { .port = DEVICE_DT_GET(DT_NODELABEL(gpiob)), .pin = 14, .dt_flags = 0 },
    { .port = DEVICE_DT_GET(DT_NODELABEL(gpiob)), .pin = 15, .dt_flags = 0 }
};

static const struct pwm_dt_spec left_enable =
    PWM_DT_SPEC_GET_BY_IDX(DT_NODELABEL(steering_servo), 1);

static const struct pwm_dt_spec right_enable =
    PWM_DT_SPEC_GET_BY_IDX(DT_NODELABEL(steering_servo), 2);

K_MUTEX_DEFINE(motor_mutex);

static bool initialized;
static bool drive_allowed;
static enum motor_mode current_mode = MOTOR_MODE_UNKNOWN;
static uint16_t current_left_duty;
static uint16_t current_right_duty;

static int set_input(enum motor_pin pin, int value)
{
    return gpio_pin_set_dt(&input_pins[pin], value);
}

static int set_enable_pulses(uint32_t left_pulse, uint32_t right_pulse)
{
    int ret = pwm_set_pulse_dt(&left_enable, left_pulse);
    if (ret < 0) {
        return ret;
    }

    ret = pwm_set_pulse_dt(&right_enable, right_pulse);
    if (ret < 0) {
        pwm_set_pulse_dt(&left_enable, 0);
        return ret;
    }

    return 0;
}

static void disable_outputs(void)
{
    pwm_set_pulse_dt(&left_enable, 0);
    pwm_set_pulse_dt(&right_enable, 0);
}

static int brake_locked(void)
{
    if (current_mode == MOTOR_MODE_BRAKE) {
        return 0;
    }

    int ret = set_enable_pulses(0, 0);
    if (ret < 0) {
        return ret;
    }

    // Equal inputs with enable high select L298 braking
    ret = set_input(IN1, 0);
    if (ret < 0) {
        return ret;
    }

    ret = set_input(IN2, 0);
    if (ret < 0) {
        return ret;
    }

    ret = set_input(IN3, 0);
    if (ret < 0) {
        return ret;
    }

    ret = set_input(IN4, 0);
    if (ret < 0) {
        return ret;
    }

    ret = set_enable_pulses(left_enable.period, right_enable.period);
    if (ret < 0) {
        disable_outputs();
        return ret;
    }

    current_mode = MOTOR_MODE_BRAKE;
    current_left_duty = 0;
    current_right_duty = 0;
    return 0;
}

static int coast_locked(void)
{
    if (current_mode == MOTOR_MODE_COAST) {
        return 0;
    }

    int ret = set_enable_pulses(0, 0);
    if (ret < 0) {
        return ret;
    }

    current_mode = MOTOR_MODE_COAST;
    current_left_duty = 0;
    current_right_duty = 0;
    return 0;
}

static int drive_locked(uint16_t left_duty, uint16_t right_duty)
{
    if (current_mode == MOTOR_MODE_DRIVE &&
        current_left_duty == left_duty &&
        current_right_duty == right_duty) {
        return 0;
    }

    if (current_mode != MOTOR_MODE_DRIVE) {
        int ret = set_enable_pulses(0, 0);
        if (ret < 0) {
            return ret;
        }

        // Left motor forward in the vehicle frame
        ret = set_input(IN1, 0);
        if (ret < 0) {
            return ret;
        }

        ret = set_input(IN2, 1);
        if (ret < 0) {
            return ret;
        }

        // Right motor forward in the vehicle frame
        ret = set_input(IN3, 1);
        if (ret < 0) {
            return ret;
        }

        ret = set_input(IN4, 0);
        if (ret < 0) {
            return ret;
        }
    }

    uint32_t left_pulse =
        (uint32_t)((uint64_t)left_enable.period * left_duty / 1000U);

    uint32_t right_pulse =
        (uint32_t)((uint64_t)right_enable.period * right_duty / 1000U);

    int ret = set_enable_pulses(left_pulse, right_pulse);
    if (ret < 0) {
        disable_outputs();
        return ret;
    }

    current_mode = MOTOR_MODE_DRIVE;
    current_left_duty = left_duty;
    current_right_duty = right_duty;
    return 0;
}

int motor_init(void)
{
    if (!device_is_ready(left_enable.dev) ||
        !device_is_ready(right_enable.dev)) {
        return -ENODEV;
    }

    int ret = set_enable_pulses(0, 0);
    if (ret < 0) {
        return ret;
    }

    for (size_t i = 0; i < 4; i++) {
        if (!gpio_is_ready_dt(&input_pins[i])) {
            return -ENODEV;
        }

        ret = gpio_pin_configure_dt(&input_pins[i],
                                    GPIO_OUTPUT_INACTIVE);
        if (ret < 0) {
            return ret;
        }
    }

    drive_allowed = false;
    current_mode = MOTOR_MODE_UNKNOWN;

    ret = brake_locked();
    if (ret < 0) {
        disable_outputs();
        return ret;
    }

    initialized = true;
    return 0;
}

int motor_set_drive_allowed(bool allowed)
{
    k_mutex_lock(&motor_mutex, K_FOREVER);

    if (!initialized) {
        k_mutex_unlock(&motor_mutex);
        return -ENODEV;
    }

    int ret;

    if (allowed) {
        ret = coast_locked();

        if (ret == 0) {
            drive_allowed = true;
        }
    } else {
        drive_allowed = false;
        ret = brake_locked();

        if (ret < 0) {
            disable_outputs();
            current_mode = MOTOR_MODE_UNKNOWN;
        }
    }

    k_mutex_unlock(&motor_mutex);
    return ret;
}

int motor_brake(void)
{
    k_mutex_lock(&motor_mutex, K_FOREVER);

    if (!initialized) {
        k_mutex_unlock(&motor_mutex);
        return -ENODEV;
    }

    int ret = brake_locked();

    if (ret < 0) {
        disable_outputs();
        current_mode = MOTOR_MODE_UNKNOWN;
    }

    k_mutex_unlock(&motor_mutex);
    return ret;
}

int motor_set_duties_permille(uint16_t left_duty, uint16_t right_duty)
{
    if (left_duty > 1000 || right_duty > 1000) {
        return -EINVAL;
    }

    k_mutex_lock(&motor_mutex, K_FOREVER);

    if (!initialized) {
        k_mutex_unlock(&motor_mutex);
        return -ENODEV;
    }

    if (!drive_allowed) {
        k_mutex_unlock(&motor_mutex);
        return -EPERM;
    }

    int ret = (left_duty == 0 && right_duty == 0)
        ? coast_locked()
        : drive_locked(left_duty, right_duty);

    if (ret < 0) {
        disable_outputs();
        current_mode = MOTOR_MODE_UNKNOWN;
    }

    k_mutex_unlock(&motor_mutex);
    return ret;
}

int motor_set_duty_permille(uint16_t duty)
{
    return motor_set_duties_permille(duty, duty);
}

int motor_set_enabled(bool enabled)
{
    return motor_set_duty_permille(enabled ? 1000 : 0);
}