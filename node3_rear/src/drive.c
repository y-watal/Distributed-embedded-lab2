/*
 * N3 application: Lab 2's motor control, split into the template's two tasks.
 *
 *  DRIVE (PRIO_DRIVE) wakes on every valid CTRL, on a rejected CTRL or a test
 *  button press, and at least every ACTUATOR_POLL_MS. It decides the mode: OFF
 *  (dynamic brake, drive not allowed: the fail-safe state) unless the zone is
 *  NORMAL and CTRL is fresh; BRAKE when brake >=
 *  CTRL_BRAKE_ACTIVE (brake wins over throttle, applied immediately - this is
 *  the end of the brake chain); COAST at zero throttle; otherwise PID with a
 *  target speed from the throttle.
 *
 *  PID (PRIO_PID) runs every PID_PERIOD_MS: encoders -> Lab 2's feedforward +
 *  PI loop -> motor duty, only while the mode is PID.
 *
 * drive_lock guards the mode decision and every motor command, so PID can't
 * write a duty after DRIVE has braked. PID computes outside the lock; the
 * lock is held only to read the mode and to write the result (the blocking
 * term DRIVE sees from PID).
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#include "app.h"
#include "catalog.h"
#include "ctrl.h"
#include "current_sensor.h"
#include "encoder.h"
#include "hb.h"
#include "lab3_config.h"
#include "motor.h"
#include "watchdog.h"

#define DRIVE_STACK_SIZE 1024
#define PID_STACK_SIZE   1536

/* Lab 2 tuning */
#define MAX_TARGET_SPEED_MM_S  650.0f /* measured speed at about 70 % PWM */
#define PID_KP                 1.0f
#define PID_KI                 2.0f
#define PID_KD                 0.0f
#define FEEDFORWARD_START_DUTY 550.0f /* full throttle starts near 70 % */
#define FEEDFORWARD_FULL_DUTY  700.0f
#define MAX_OUTPUT_DUTY        1000.0f
#define PID_INTEGRAL_LIMIT     400.0f

/* Lab 2 brake threshold 500 raw -> CTRL counts (raw >> 7, rounded up). */
#define CTRL_BRAKE_ACTIVE      4
#define CTRL_THROTTLE_DEADBAND 2   /* Lab 2's 300 raw */
#define CTRL_PEDAL_MAX         255

enum drive_mode { DRIVE_OFF, DRIVE_COAST, DRIVE_BRAKE, DRIVE_PID };
static const char *const mode_names[] = {"OFF", "COAST", "BRAKE", "PID"};

struct speed_pid {
	float integral;
	float previous_speed;
};

K_MUTEX_DEFINE(drive_lock);
static enum drive_mode mode = DRIVE_OFF; /* drive_lock */
static float target_mm_s;                 /* drive_lock */
static uint32_t pid_epoch;                /* drive_lock: bumped when PID must restart */

/* Telemetry for the report thread. */
static atomic_t t_mode = ATOMIC_INIT(DRIVE_OFF);
static atomic_t t_target, t_speed_l, t_speed_r, t_duty_l, t_duty_r, t_encoder_err;

K_THREAD_STACK_DEFINE(drive_stack, DRIVE_STACK_SIZE);
K_THREAD_STACK_DEFINE(pid_stack, PID_STACK_SIZE);
static struct k_thread drive_data, pid_data;

static float clamp_float(float value, float minimum, float maximum)
{
	return value < minimum ? minimum : (value > maximum ? maximum : value);
}

static float throttle_to_target_speed(uint8_t throttle)
{
	if (throttle <= CTRL_THROTTLE_DEADBAND) {
		return 0.0f;
	}
	return (float)(throttle - CTRL_THROTTLE_DEADBAND) * MAX_TARGET_SPEED_MM_S /
	       (CTRL_PEDAL_MAX - CTRL_THROTTLE_DEADBAND);
}

static void reset_pid(struct speed_pid *pid)
{
	pid->integral = 0.0f;
	pid->previous_speed = 0.0f;
}

/* Lab 2's speed_to_duty(), unchanged. */
static uint16_t speed_to_duty(struct speed_pid *pid, float target, float measured, float elapsed_s)
{
	float error = target - measured;
	float feedforward = FEEDFORWARD_START_DUTY + (FEEDFORWARD_FULL_DUTY - FEEDFORWARD_START_DUTY) *
							     target / MAX_TARGET_SPEED_MM_S;
	float speed_derivative = (measured - pid->previous_speed) / elapsed_s;
	float next_integral = clamp_float(pid->integral + PID_KI * error * elapsed_s,
					  -PID_INTEGRAL_LIMIT, PID_INTEGRAL_LIMIT);
	float output = feedforward + PID_KP * error + next_integral - PID_KD * speed_derivative;

	/* Hold the integral when output is saturated in the error direction. */
	if (!((output > MAX_OUTPUT_DUTY && error > 0.0f) || (output < 0.0f && error < 0.0f))) {
		pid->integral = next_integral;
	}
	output = feedforward + PID_KP * error + pid->integral - PID_KD * speed_derivative;
	pid->previous_speed = measured;
	return (uint16_t)clamp_float(output, 0.0f, MAX_OUTPUT_DUTY);
}

static void set_mode_locked(enum drive_mode m)
{
	if (m != mode && (m == DRIVE_PID || mode == DRIVE_PID)) {
		pid_epoch++;
	}
	mode = m;
	atomic_set(&t_mode, m);
	if (m != DRIVE_PID) {
		atomic_set(&t_duty_l, 0);
		atomic_set(&t_duty_r, 0);
	}
}

static void drive_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		struct ctrl_cmd cmd;
		uint32_t age;

		ctrl_wait(K_MSEC(ACTUATOR_POLL_MS));

		bool ok = hb_zone_state() == ZONE_NORMAL && ctrl_get(&cmd, &age) &&
			  age < CTRL_TIMEOUT_MS;

		k_mutex_lock(&drive_lock, K_FOREVER);
		if (!ok) {
			if (mode != DRIVE_OFF) {
				motor_set_drive_allowed(false); /* dynamic brake */
				set_mode_locked(DRIVE_OFF);
			}
			target_mm_s = 0.0f;
		} else {
			if (mode == DRIVE_OFF && motor_set_drive_allowed(true) == 0) {
				set_mode_locked(DRIVE_COAST);
			}
			if (mode != DRIVE_OFF) {
				if (cmd.brake >= CTRL_BRAKE_ACTIVE) {
					motor_brake(); /* brake wins over throttle and PID */
					set_mode_locked(DRIVE_BRAKE);
					target_mm_s = 0.0f;
				} else {
					target_mm_s = throttle_to_target_speed(cmd.throttle);
					if (target_mm_s == 0.0f) {
						motor_set_duties_permille(0, 0); /* coast */
						set_mode_locked(DRIVE_COAST);
					} else {
						set_mode_locked(DRIVE_PID);
					}
				}
			}
		}
		atomic_set(&t_target, (atomic_val_t)target_mm_s);
		k_mutex_unlock(&drive_lock);
		watchdog_checkin(WDT_ACTUATOR);
	}
}

static void pid_thread(void *p1, void *p2, void *p3)
{
	struct speed_pid left_pid = {0}, right_pid = {0};
	struct encoder_readings rd;
	int64_t next = k_uptime_get();
	int64_t last_sample_ms = 0;
	uint32_t epoch_seen = 0;
	bool have_sample = false;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		next += PID_PERIOD_MS;
		k_sleep(K_TIMEOUT_ABS_MS(next));

		k_mutex_lock(&drive_lock, K_FOREVER);
		bool active = mode == DRIVE_PID;
		float target = target_mm_s;
		uint32_t epoch = pid_epoch;
		k_mutex_unlock(&drive_lock);

		if (!active || epoch != epoch_seen) {
			reset_pid(&left_pid);
			reset_pid(&right_pid);
			have_sample = false;
			epoch_seen = epoch;
			if (!active) {
				continue;
			}
		}

		int64_t now_ms = k_uptime_get();

		if (encoder_read(&rd) < 0) {
			atomic_inc(&t_encoder_err);
			k_mutex_lock(&drive_lock, K_FOREVER);
			motor_brake();
			set_mode_locked(DRIVE_BRAKE);
			k_mutex_unlock(&drive_lock);
			continue;
		}
		atomic_set(&t_speed_l, rd.left_velocity_mm_s);
		atomic_set(&t_speed_r, rd.right_velocity_mm_s);

		if (!have_sample) { /* first read only sets the velocity baseline */
			last_sample_ms = now_ms;
			have_sample = true;
			continue;
		}

		float elapsed_s = (float)(now_ms - last_sample_ms) / 1000.0f;

		last_sample_ms = now_ms;
		uint16_t duty_l = speed_to_duty(&left_pid, target, (float)rd.left_velocity_mm_s, elapsed_s);
		uint16_t duty_r = speed_to_duty(&right_pid, target, (float)rd.right_velocity_mm_s, elapsed_s);

		/* Write only if DRIVE hasn't changed the mode while we computed. */
		k_mutex_lock(&drive_lock, K_FOREVER);
		if (mode == DRIVE_PID && pid_epoch == epoch_seen &&
		    motor_set_duties_permille(duty_l, duty_r) == 0) {
			atomic_set(&t_duty_l, duty_l);
			atomic_set(&t_duty_r, duty_r);
		}
		k_mutex_unlock(&drive_lock);
	}
}

void app_on_frame(const struct can_frame *frame, int bus)
{
	/* CTRL, BLINK, HB and FAULT are routed in main.c; nothing else for N3. */
	ARG_UNUSED(frame);
	ARG_UNUSED(bus);
}

int app_init(void)
{
	/* Lab 2 order: motors braked first, then calibrate with no load current. */
	int ret = motor_init();

	if (ret < 0) {
		printk("motor init failed (%d)\n", ret);
		return ret;
	}
	ret = current_sensor_init();
	if (ret < 0) {
		printk("current sensor init failed (%d)\n", ret);
	}
	ret = encoder_init();
	if (ret < 0) {
		printk("encoder init failed (%d)\n", ret);
		return ret;
	}

	k_thread_create(&pid_data, pid_stack, K_THREAD_STACK_SIZEOF(pid_stack), pid_thread, NULL,
			NULL, NULL, PRIO_PID, 0, K_NO_WAIT);
	k_thread_name_set(&pid_data, "pid");
	k_thread_create(&drive_data, drive_stack, K_THREAD_STACK_SIZEOF(drive_stack),
			drive_thread, NULL, NULL, NULL, PRIO_DRIVE, 0, K_NO_WAIT);
	k_thread_name_set(&drive_data, "drive");
	return 0;
}

void app_report(void)
{
	int32_t ma;

	printk("  drive: %s target %ld mm/s | speed L %ld R %ld mm/s | duty L %ld R %ld /1000",
	       mode_names[atomic_get(&t_mode)], (long)atomic_get(&t_target),
	       (long)atomic_get(&t_speed_l), (long)atomic_get(&t_speed_r),
	       (long)atomic_get(&t_duty_l), (long)atomic_get(&t_duty_r));
	if (atomic_get(&t_encoder_err)) {
		printk(" | encoder errors %ld", (long)atomic_get(&t_encoder_err));
	}
	for (int i = 0; i < current_sensor_count(); i++) {
		if (current_sensor_get_ma(i, &ma)) {
			printk(" | I %s %d mA", current_sensor_name(i), (int)ma);
		}
	}
	printk("\n");
}
