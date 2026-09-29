#include "safety.h"

#include "app_state.h"
#include "blinkers.h"
#include "led.h"
#include "motor.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#include <stdbool.h>
#include <stdint.h>

#define SAFETY_CHECK_MS 5
#define COMMAND_STALE_MS 90
#define SAFETY_STACK_SIZE 1024
#define SAFETY_PRIORITY 2

enum self_test_request {
    SELF_TEST_REQUEST_NONE,
    SELF_TEST_REQUEST_ENTER,
    SELF_TEST_REQUEST_EXIT
};

static atomic_t car_state = ATOMIC_INIT(CAR_STATE_ERROR);
static atomic_t fault_reason_value = ATOMIC_INIT(FAULT_LINK_TIMEOUT);
static atomic_t self_test_request = ATOMIC_INIT(SELF_TEST_REQUEST_NONE);

K_THREAD_STACK_DEFINE(safety_stack, SAFETY_STACK_SIZE);
static struct k_thread safety_thread_data;

enum car_state safety_get_state(void)
{
    return (enum car_state)atomic_get(&car_state);
}

enum fault_reason safety_get_fault(void)
{
    return (enum fault_reason)atomic_get(&fault_reason_value);
}

void safety_bad_range(void)
{
    /* Reject the previous command too: recovery requires a new valid frame. */
    app_state_invalidate_command();
    atomic_set(&car_state, CAR_STATE_ERROR);
    atomic_set(&fault_reason_value, FAULT_BAD_RANGE);
    int ret = motor_set_drive_allowed(false);
    led_hazards_start();
    printk("state=ERROR fault=BAD_RANGE brake_result=%d\n", ret);
}

void safety_request_self_test_enter(void)
{
    atomic_set(&self_test_request, SELF_TEST_REQUEST_ENTER);
}

void safety_request_self_test_exit(void)
{
    atomic_set(&self_test_request, SELF_TEST_REQUEST_EXIT);
}

static bool command_is_fresh(void)
{
    struct wheel_command command;

    if (!app_state_get_command(&command)) {
        return false;
    }

    int64_t age_ms = k_uptime_get() - command.received_at_ms;

    return age_ms >= 0 && age_ms < COMMAND_STALE_MS;
}

static void show_indicator(enum indicator_mode mode)
{
    switch (mode) {
    case INDICATOR_LEFT:
        led_left_start();
        break;

    case INDICATOR_RIGHT:
        led_right_start();
        break;

    case INDICATOR_OFF:
    default:
        led_all_off();
        break;
    }
}

static void safety_thread(void *arg1, void *arg2, void *arg3)
{
    int applied_indicator = -1;

    (void)arg1;
    (void)arg2;
    (void)arg3;

    while (1) {
        bool fresh = command_is_fresh();
        enum car_state current = safety_get_state();

        enum self_test_request request =
            (enum self_test_request)atomic_set(
                &self_test_request, SELF_TEST_REQUEST_NONE
            );

        if (!fresh) {
            if (current != CAR_STATE_ERROR) {
                // Revoke drive permission before changing the lights
                atomic_set(&car_state, CAR_STATE_ERROR);
                atomic_set(&fault_reason_value, FAULT_LINK_TIMEOUT);
                int ret = motor_set_drive_allowed(false);
                led_hazards_start();
                applied_indicator = -1;

                printk("state=ERROR link=TIMEOUT brake_result=%d\n", ret);
            }

            k_sleep(K_MSEC(SAFETY_CHECK_MS));
            continue;
        }

        if (current == CAR_STATE_ERROR) {
            int ret = motor_set_drive_allowed(true);

            if (ret < 0) {
                printk("ERROR: could not release motor brake: %d\n", ret);
            } else {
                atomic_set(&car_state, CAR_STATE_NORMAL);
                atomic_set(&fault_reason_value, FAULT_NONE);
                current = CAR_STATE_NORMAL;
                applied_indicator = -1;
                printk("state=NORMAL link=OK\n");
            }
        }

        if (current == CAR_STATE_NORMAL &&
            request == SELF_TEST_REQUEST_ENTER) {
            // Block new motor commands before selecting the brake
            atomic_set(&car_state, CAR_STATE_SELF_TEST);
            atomic_set(&fault_reason_value, FAULT_SELF_TEST);
            int ret = motor_set_drive_allowed(false);
            led_hazards_start();
            applied_indicator = -1;

            printk("state=SELF_TEST brake_result=%d\n", ret);
            k_sleep(K_MSEC(SAFETY_CHECK_MS));
            continue;
        }

        if (current == CAR_STATE_SELF_TEST) {
            if (request == SELF_TEST_REQUEST_EXIT) {
                int ret = motor_set_drive_allowed(true);

                if (ret < 0) {
                    printk("ERROR: could not exit self-test: %d\n", ret);
                } else {
                    atomic_set(&car_state, CAR_STATE_NORMAL);
                    atomic_set(&fault_reason_value, FAULT_NONE);
                    current = CAR_STATE_NORMAL;
                    applied_indicator = -1;
                    printk("state=NORMAL self_test=EXIT\n");
                }
            } else {
                k_sleep(K_MSEC(SAFETY_CHECK_MS));
                continue;
            }
        }

        if (current == CAR_STATE_NORMAL) {
            enum indicator_mode desired = blinkers_get_mode();

            if ((int)desired != applied_indicator) {
                show_indicator(desired);
                applied_indicator = (int)desired;
            }
        }

        k_sleep(K_MSEC(SAFETY_CHECK_MS));
    }
}

void safety_start(void)
{
    k_thread_create(&safety_thread_data, safety_stack,
                    K_THREAD_STACK_SIZEOF(safety_stack),
                    safety_thread, NULL, NULL, NULL,
                    SAFETY_PRIORITY, 0, K_NO_WAIT);
}
