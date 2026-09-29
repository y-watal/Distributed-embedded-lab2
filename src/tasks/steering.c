#include "steering.h"

#include "app_state.h"
#include "safety.h"
#include "servo.h"

#include <zephyr/kernel.h>

#include <stdint.h>

#define SERVO_LEFT_ANGLE_DEG   0
#define SERVO_CENTER_ANGLE_DEG 90
#define SERVO_RIGHT_ANGLE_DEG  180

#define STEERING_MAX_INPUT 10000

#define STEERING_PERIOD_MS 20
#define STEERING_STACK_SIZE 1024
#define STEERING_PRIORITY 5

K_THREAD_STACK_DEFINE(steering_stack, STEERING_STACK_SIZE);
static struct k_thread steering_thread_data;

static int wheel_to_servo_angle(int32_t steering)
{
    if (steering < -STEERING_MAX_INPUT) {
        steering = -STEERING_MAX_INPUT;
    } else if (steering > STEERING_MAX_INPUT) {
        steering = STEERING_MAX_INPUT;
    }

    if (steering < 0) {
        return SERVO_CENTER_ANGLE_DEG +
            (int)(steering *
                  (SERVO_CENTER_ANGLE_DEG - SERVO_LEFT_ANGLE_DEG) /
                  STEERING_MAX_INPUT);
    }

    return SERVO_CENTER_ANGLE_DEG +
        (int)(steering *
              (SERVO_RIGHT_ANGLE_DEG - SERVO_CENTER_ANGLE_DEG) /
              STEERING_MAX_INPUT);
}

static void steering_thread(void *arg1, void *arg2, void *arg3)
{
    int last_angle = -1;

    (void)arg1;
    (void)arg2;
    (void)arg3;

    while (1) {
        int angle = SERVO_CENTER_ANGLE_DEG;
        struct wheel_command command;

        if (safety_get_state() == CAR_STATE_NORMAL &&
            app_state_get_command(&command)) {
            angle = wheel_to_servo_angle(command.steering);
        }

        if (angle != last_angle && servo_set_angle(angle) == 0) {
            last_angle = angle;
        }

        k_sleep(K_MSEC(STEERING_PERIOD_MS));
    }
}

void steering_start(void)
{
    k_thread_create(&steering_thread_data, steering_stack,
                    K_THREAD_STACK_SIZEOF(steering_stack),
                    steering_thread, NULL, NULL, NULL,
                    STEERING_PRIORITY, 0, K_NO_WAIT);
}