/*
 * N2 application: STEER (Lab 2's steering task, now driven by CTRL over CAN),
 * FORCE_TX, and the servo current sensor.
 *
 * STEER wakes on every valid CTRL (and at least every ACTUATOR_POLL_MS). The
 * servo follows the wheel only while the zone is NORMAL and CTRL is fresh;
 * otherwise it returns to center, as in Lab 2. The servo duty is set once
 * per CTRL acted on (even if the angle is the same) and whenever the angle
 * changes otherwise (re-centering); poll wake-ups with nothing new don't
 * touch it. Each set toggles SRV, so SRV marks every CTRL that reached the
 * servo.
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#include "app.h"
#include "canbus.h"
#include "catalog.h"
#include "ctrl.h"
#include "current_sensor.h"
#include "hb.h"
#include "lab3_config.h"
#include "servo.h"
#include "watchdog.h"

#define STEER_STACK_SIZE 1024
#define FORCE_STACK_SIZE 768

/* Lab 2: wheel input saturates at +-10000 raw (full lock). */
#define STEERING_MAX_INPUT 10000

K_THREAD_STACK_DEFINE(steer_stack, STEER_STACK_SIZE);
K_THREAD_STACK_DEFINE(force_stack, FORCE_STACK_SIZE);
static struct k_thread steer_data, force_data;
static atomic_t force_value;

static atomic_t angle_now = ATOMIC_INIT(SERVO_CENTER_DEG);
static atomic_t following; /* 1 while the servo follows the wheel */

static int wheel_to_servo_angle(int32_t steering)
{
	steering = CLAMP(steering, -STEERING_MAX_INPUT, STEERING_MAX_INPUT);
	if (steering < 0) {
		return SERVO_CENTER_DEG +
		       (int)(steering * (SERVO_CENTER_DEG - SERVO_LEFT_DEG) / STEERING_MAX_INPUT);
	}
	return SERVO_CENTER_DEG +
	       (int)(steering * (SERVO_RIGHT_DEG - SERVO_CENTER_DEG) / STEERING_MAX_INPUT);
}

static void steer_thread(void *p1, void *p2, void *p3)
{
	int last_angle = -1;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		struct ctrl_cmd cmd;
		uint32_t age;
		int angle = SERVO_CENTER_DEG;

		/* true: a CTRL arrived (or a reject / kick, which takes the zone out
		 * of NORMAL first, so ok is false and the servo re-centers). */
		bool woke = ctrl_wait(K_MSEC(ACTUATOR_POLL_MS));

		bool ok = hb_zone_state() == ZONE_NORMAL && ctrl_get(&cmd, &age) &&
			  age < CTRL_TIMEOUT_MS;

		if (ok) {
			angle = wheel_to_servo_angle(cmd.steer);
		}
		atomic_set(&following, ok);

		if ((angle != last_angle || (ok && woke)) && servo_set_angle(angle) == 0) {
			last_angle = angle;
			atomic_set(&angle_now, angle);
		}
		watchdog_checkin(WDT_ACTUATOR);
	}
}

/* FORCE_TX: force feedback to the cockpit every 50 ms. The payload is a dummy
 * counter in this lab (byte 1, int8, wraps). */
static void force_thread(void *p1, void *p2, void *p3)
{
	int64_t next = k_uptime_get();
	int8_t force = 0;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		uint8_t p[FRAME_DLC] = {0};

		next += FORCE_PERIOD_MS;
		k_sleep(K_TIMEOUT_ABS_MS(next));
		force = (int8_t)(force + 1);
		p[1] = (uint8_t)force;
		canbus_send(ID_FORCE, p, false);
		atomic_set(&force_value, force);
	}
}

void app_on_frame(const struct can_frame *frame, int bus)
{
	/* CTRL, BLINK, HB and FAULT are routed in main.c; nothing else for N2. */
	ARG_UNUSED(frame);
	ARG_UNUSED(bus);
}

int app_init(void)
{
	/* Lab 2 order: calibrate the current sensor before the servo draws current. */
	int ret = current_sensor_init();

	if (ret < 0) {
		printk("current sensor init failed (%d)\n", ret);
	}

	ret = servo_init();
	if (ret < 0) {
		printk("servo init failed (%d)\n", ret);
		return ret;
	}

	k_thread_create(&steer_data, steer_stack, K_THREAD_STACK_SIZEOF(steer_stack),
			steer_thread, NULL, NULL, NULL, PRIO_STEER, 0, K_NO_WAIT);
	k_thread_name_set(&steer_data, "steer");
	k_thread_create(&force_data, force_stack, K_THREAD_STACK_SIZEOF(force_stack),
			force_thread, NULL, NULL, NULL, PRIO_FORCE, 0, K_NO_WAIT);
	k_thread_name_set(&force_data, "force");
	return 0;
}

void app_report(void)
{
	int32_t ma;

	printk("  steer: %s angle %d deg", atomic_get(&following) ? "following" : "centered",
	       (int)atomic_get(&angle_now));
	for (int i = 0; i < current_sensor_count(); i++) {
		if (current_sensor_get_ma(i, &ma)) {
			printk(" | %s %d mA", current_sensor_name(i), (int)ma);
		}
	}
	printk(" | FORCE %d\n", (int)atomic_get(&force_value));
}
