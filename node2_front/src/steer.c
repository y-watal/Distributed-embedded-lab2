/*
 * N2 application: STEER (Lab 2's steering task, now driven by CTRL over CAN)
 * and the servo current sensor.
 *
 * STEER wakes on every valid CTRL (and at least every ACTUATOR_POLL_MS). The
 * servo follows the wheel only while the zone is NORMAL and CTRL is fresh;
 * otherwise it returns to center, as in Lab 2.
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#include "app.h"
#include "catalog.h"
#include "ctrl.h"
#include "current_sensor.h"
#include "hb.h"
#include "lab3_config.h"
#include "servo.h"

#define STEER_STACK_SIZE 1024

/* Lab 2: wheel input saturates at +-10000 raw (full lock). */
#define STEERING_MAX_INPUT 10000

K_THREAD_STACK_DEFINE(steer_stack, STEER_STACK_SIZE);
static struct k_thread steer_data;

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

		ctrl_wait(K_MSEC(ACTUATOR_POLL_MS));

		bool ok = hb_zone_state() == ZONE_NORMAL && ctrl_get(&cmd, &age) &&
			  age < CTRL_TIMEOUT_MS;

		if (ok) {
			angle = wheel_to_servo_angle(cmd.steer);
		}
		atomic_set(&following, ok);

		if (angle != last_angle && servo_set_angle(angle) == 0) {
			last_angle = angle;
			atomic_set(&angle_now, angle);
		}
	}
}

void app_on_frame(const struct can_frame *frame, int bus)
{
	ARG_UNUSED(bus);
	if (frame->id == ID_CTRL) {
		ctrl_on_frame(frame);
	}
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
	printk("\n");
}
