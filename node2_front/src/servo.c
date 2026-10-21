/*
 * Ported from Lab 2 src/drivers/servo.c; the PWM comes from zephyr,user "servo".
 * Each successful servo_set_angle() toggles the SRV test point (D14 / PB9),
 * even if the angle didn't change; servo_init() doesn't.
 */
#include "servo.h"

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/pwm.h>

#include "tp.h"

#define SERVO_MIN_PULSE_US 500
#define SERVO_MAX_PULSE_US 2500

static const struct pwm_dt_spec servo_pwm = PWM_DT_SPEC_GET_BY_NAME(DT_PATH(zephyr_user), servo);

static int set_angle(int angle_deg)
{
	if (!pwm_is_ready_dt(&servo_pwm)) {
		return -ENODEV;
	}
	if (angle_deg < SERVO_LEFT_DEG || angle_deg > SERVO_RIGHT_DEG) {
		return -EINVAL;
	}

	uint32_t pulse_us = SERVO_MIN_PULSE_US + (uint32_t)angle_deg *
						       (SERVO_MAX_PULSE_US - SERVO_MIN_PULSE_US) /
						       SERVO_RIGHT_DEG;

	return pwm_set_pulse_dt(&servo_pwm, PWM_USEC(pulse_us));
}

int servo_set_angle(int angle_deg)
{
	int ret = set_angle(angle_deg);

	if (ret == 0) {
		tp_toggle(TP_SRV); /* servo duty set */
	}
	return ret;
}

int servo_init(void)
{
	return set_angle(SERVO_CENTER_DEG);
}
