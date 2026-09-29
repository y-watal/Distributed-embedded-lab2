#include "motor_control.h"

#include "app_state.h"
#include "encoder.h"
#include "motor.h"
#include "safety.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#include <stdbool.h>
#include <stdint.h>

#define CONTROL_PERIOD_MS 20
#define COMMAND_STALE_MS 90
#define DEBUG_PERIOD_MS 250

#define THROTTLE_DEADBAND 300
#define BRAKE_ACTIVE_THRESHOLD 500

// Set this to the measured speed at approximately 70 percent PWM
#define MAX_TARGET_SPEED_MM_S 650.0f

// Starting gains
#define PID_KP 1.0f
#define PID_KI 2.0f
#define PID_KD 0.0f

// Full throttle starts near 70 percent before PID correction
#define FEEDFORWARD_START_DUTY 550.0f
#define FEEDFORWARD_FULL_DUTY 700.0f

// PID may use the remaining 30 percent to recover from load
#define MAX_OUTPUT_DUTY 1000.0f
#define PID_INTEGRAL_LIMIT 400.0f

#define MOTOR_CONTROL_STACK_SIZE 1536
#define MOTOR_CONTROL_PRIORITY 1
#define DEBUG_STACK_SIZE 1024
#define DEBUG_PRIORITY 6

enum debug_mode {
    DEBUG_ERROR,
    DEBUG_COAST,
    DEBUG_BRAKE,
    DEBUG_PID,
    DEBUG_ENCODER_ERROR,
    DEBUG_OUTPUT_ERROR
};

struct speed_pid {
    float integral;
    float previous_speed;
};

K_THREAD_STACK_DEFINE(motor_control_stack, MOTOR_CONTROL_STACK_SIZE);
K_THREAD_STACK_DEFINE(debug_stack, DEBUG_STACK_SIZE);

static struct k_thread motor_control_thread_data;
static struct k_thread debug_thread_data;

static atomic_t debug_mode_value = ATOMIC_INIT(DEBUG_ERROR);
static atomic_t debug_throttle = ATOMIC_INIT(0);
static atomic_t debug_brake = ATOMIC_INIT(0);
static atomic_t debug_target = ATOMIC_INIT(0);
static atomic_t debug_left_speed = ATOMIC_INIT(0);
static atomic_t debug_right_speed = ATOMIC_INIT(0);
static atomic_t debug_left_duty = ATOMIC_INIT(0);
static atomic_t debug_right_duty = ATOMIC_INIT(0);
static atomic_t debug_command_age = ATOMIC_INIT(0);

static float clamp_float(float value, float minimum, float maximum)
{
    if (value < minimum) {
        return minimum;
    }

    if (value > maximum) {
        return maximum;
    }

    return value;
}

static float throttle_to_target_speed(int32_t throttle)
{
    if (throttle <= THROTTLE_DEADBAND) {
        return 0.0f;
    }

    if (throttle > 32767) {
        throttle = 32767;
    }

    return (float)(throttle - THROTTLE_DEADBAND) *
        MAX_TARGET_SPEED_MM_S / (32767 - THROTTLE_DEADBAND);
}

static void reset_pid(struct speed_pid *pid)
{
    pid->integral = 0.0f;
    pid->previous_speed = 0.0f;
}

static uint16_t speed_to_duty(struct speed_pid *pid, float target,
                              float measured, float elapsed_s)
{
    float error = target - measured;
    float feedforward = FEEDFORWARD_START_DUTY +
        (FEEDFORWARD_FULL_DUTY - FEEDFORWARD_START_DUTY) *
        target / MAX_TARGET_SPEED_MM_S;

    float speed_derivative =
        (measured - pid->previous_speed) / elapsed_s;

    float next_integral = clamp_float(
        pid->integral + PID_KI * error * elapsed_s,
        -PID_INTEGRAL_LIMIT, PID_INTEGRAL_LIMIT
    );

    float output = feedforward + PID_KP * error + next_integral -
        PID_KD * speed_derivative;

    // Hold the integral when output is saturated in the error direction
    if (!((output > MAX_OUTPUT_DUTY && error > 0.0f) ||
          (output < 0.0f && error < 0.0f))) {
        pid->integral = next_integral;
    }

    output = feedforward + PID_KP * error + pid->integral -
        PID_KD * speed_derivative;

    pid->previous_speed = measured;
    return (uint16_t)clamp_float(output, 0.0f, MAX_OUTPUT_DUTY);
}

static void set_debug_inputs(const struct wheel_command *command,
                             int64_t age_ms)
{
    atomic_set(&debug_throttle, command->throttle);
    atomic_set(&debug_brake, command->brake);
    atomic_set(&debug_command_age, (atomic_val_t)age_ms);
}

static void clear_debug_output(enum debug_mode mode)
{
    atomic_set(&debug_mode_value, mode);
    atomic_set(&debug_target, 0);
    atomic_set(&debug_left_duty, 0);
    atomic_set(&debug_right_duty, 0);
}

static void motor_control_thread(void *arg1, void *arg2, void *arg3)
{
    struct speed_pid left_pid = {0};
    struct speed_pid right_pid = {0};
    struct encoder_readings readings;
    struct wheel_command command;

    bool have_speed_sample = false;
    int64_t last_sample_ms = 0;

    (void)arg1;
    (void)arg2;
    (void)arg3;

    while (1) {
        // New packets wake brake handling immediately
        app_state_wait_for_command_ms(CONTROL_PERIOD_MS);

        int64_t now_ms = k_uptime_get();

        if (!app_state_get_command(&command) ||
            safety_get_state() != CAR_STATE_NORMAL) {
            reset_pid(&left_pid);
            reset_pid(&right_pid);
            have_speed_sample = false;
            clear_debug_output(DEBUG_ERROR);
            continue;
        }

        int64_t command_age_ms = now_ms - command.received_at_ms;
        set_debug_inputs(&command, command_age_ms);

        if (command_age_ms < 0 ||
            command_age_ms >= COMMAND_STALE_MS) {
            motor_brake();
            reset_pid(&left_pid);
            reset_pid(&right_pid);
            have_speed_sample = false;
            clear_debug_output(DEBUG_ERROR);
            continue;
        }

        // Brake wins over throttle and the PID loop
        if (command.brake >= BRAKE_ACTIVE_THRESHOLD) {
            motor_brake();
            reset_pid(&left_pid);
            reset_pid(&right_pid);
            have_speed_sample = false;
            clear_debug_output(DEBUG_BRAKE);
            continue;
        }

        float target_speed = throttle_to_target_speed(command.throttle);

        if (target_speed == 0.0f) {
            motor_set_duties_permille(0, 0);
            reset_pid(&left_pid);
            reset_pid(&right_pid);
            have_speed_sample = false;
            clear_debug_output(DEBUG_COAST);
            continue;
        }

        atomic_set(&debug_target, (atomic_val_t)target_speed);

        if (!have_speed_sample) {
            if (encoder_read(&readings) < 0) {
                motor_brake();
                clear_debug_output(DEBUG_ENCODER_ERROR);
                continue;
            }

            last_sample_ms = now_ms;
            have_speed_sample = true;
            continue;
        }

        int64_t elapsed_ms = now_ms - last_sample_ms;

        if (elapsed_ms < CONTROL_PERIOD_MS) {
            continue;
        }

        if (encoder_read(&readings) < 0) {
            motor_brake();
            reset_pid(&left_pid);
            reset_pid(&right_pid);
            have_speed_sample = false;
            clear_debug_output(DEBUG_ENCODER_ERROR);
            continue;
        }

        last_sample_ms = now_ms;

        float elapsed_s = (float)elapsed_ms / 1000.0f;
        uint16_t left_duty = speed_to_duty(
            &left_pid, target_speed,
            (float)readings.left_velocity_mm_s, elapsed_s
        );

        uint16_t right_duty = speed_to_duty(
            &right_pid, target_speed,
            (float)readings.right_velocity_mm_s, elapsed_s
        );

        int ret = motor_set_duties_permille(left_duty, right_duty);

        atomic_set(&debug_left_speed, readings.left_velocity_mm_s);
        atomic_set(&debug_right_speed, readings.right_velocity_mm_s);

        if (ret < 0) {
            motor_brake();
            reset_pid(&left_pid);
            reset_pid(&right_pid);
            have_speed_sample = false;
            clear_debug_output(DEBUG_OUTPUT_ERROR);
            continue;
        }

        atomic_set(&debug_left_duty, left_duty);
        atomic_set(&debug_right_duty, right_duty);
        atomic_set(&debug_mode_value, DEBUG_PID);
    }
}

static void debug_thread(void *arg1, void *arg2, void *arg3)
{
    static const char *const mode_names[] = {
        "ERROR", "COAST", "BRAKE", "PID",
        "ENCODER_ERROR", "OUTPUT_ERROR"
    };

    (void)arg1;
    (void)arg2;
    (void)arg3;

    while (1) {
        enum debug_mode mode = (enum debug_mode)atomic_get(&debug_mode_value);

        printk("ctrl=%s throttle=%d brake=%d age=%dms "
               "target=%dmm/s speed_L=%d speed_R=%dmm/s "
               "duty_L=%d duty_R=%d/1000\n",
               mode_names[mode],
               (int)atomic_get(&debug_throttle),
               (int)atomic_get(&debug_brake),
               (int)atomic_get(&debug_command_age),
               (int)atomic_get(&debug_target),
               (int)atomic_get(&debug_left_speed),
               (int)atomic_get(&debug_right_speed),
               (int)atomic_get(&debug_left_duty),
               (int)atomic_get(&debug_right_duty));

        k_sleep(K_MSEC(DEBUG_PERIOD_MS));
    }
}

void motor_control_start(void)
{
    k_thread_create(&motor_control_thread_data, motor_control_stack,
                    K_THREAD_STACK_SIZEOF(motor_control_stack),
                    motor_control_thread, NULL, NULL, NULL,
                    MOTOR_CONTROL_PRIORITY, 0, K_NO_WAIT);

    k_thread_create(&debug_thread_data, debug_stack,
                    K_THREAD_STACK_SIZEOF(debug_stack),
                    debug_thread, NULL, NULL, NULL,
                    DEBUG_PRIORITY, 0, K_NO_WAIT);
}