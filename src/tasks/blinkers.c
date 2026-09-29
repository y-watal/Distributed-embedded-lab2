#include "blinkers.h"

#include "app_state.h"
#include "safety.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include <stdbool.h>
#include <stdint.h>

#define BLINKERS_CHECK_MS 5
#define BLINKERS_STACK_SIZE 1024
#define BLINKERS_PRIORITY 3

#define TURN_THRESHOLD 5000
#define RETURN_THRESHOLD 4000

#define BUTTON_DEBOUNCE_MS 30
#define SELF_TEST_DOUBLE_PRESS_MS 350

static atomic_t selected_indicator = ATOMIC_INIT(INDICATOR_OFF);

K_THREAD_STACK_DEFINE(blinkers_stack, BLINKERS_STACK_SIZE);
static struct k_thread blinkers_thread_data;

enum indicator_mode blinkers_get_mode(void)
{
    return (enum indicator_mode)atomic_get(&selected_indicator);
}

static void select_indicator(enum indicator_mode mode, bool *turn_armed)
{
    enum indicator_mode current = blinkers_get_mode();

    // Pressing the active side again turns it off
    if (current == mode) {
        mode = INDICATOR_OFF;
    }

    atomic_set(&selected_indicator, mode);
    *turn_armed = false;
}

static void blinkers_thread(void *arg1, void *arg2, void *arg3)
{
    bool previous_b4 = false;
    bool previous_b5 = false;
    bool previous_b10 = false;
    bool turn_armed = false;
    bool exit_click_pending = false;

    int64_t last_b10_press_ms = -1000;
    int64_t first_exit_click_ms = 0;

    (void)arg1;
    (void)arg2;
    (void)arg3;

    while (1) {
        struct wheel_command command;

        if (!app_state_get_command(&command)) {
            k_sleep(K_MSEC(BLINKERS_CHECK_MS));
            continue;
        }

        bool b4 = command.button4 != 0;
        bool b5 = command.button5 != 0;
        bool b10 = command.button10 != 0;

        bool right_press = b4 && !previous_b4;
        bool left_press = b5 && !previous_b5;
        bool self_test_press = b10 && !previous_b10;

        previous_b4 = b4;
        previous_b5 = b5;
        previous_b10 = b10;

        int64_t now_ms = k_uptime_get();
        enum car_state state = safety_get_state();

        if (self_test_press &&
            now_ms - last_b10_press_ms >= BUTTON_DEBOUNCE_MS) {
            last_b10_press_ms = now_ms;

            if (state == CAR_STATE_NORMAL) {
                atomic_set(&selected_indicator, INDICATOR_OFF);
                turn_armed = false;
                exit_click_pending = false;
                safety_request_self_test_enter();
            } else if (state == CAR_STATE_SELF_TEST) {
                if (exit_click_pending &&
                    now_ms - first_exit_click_ms <=
                        SELF_TEST_DOUBLE_PRESS_MS) {
                    exit_click_pending = false;
                    safety_request_self_test_exit();
                } else {
                    first_exit_click_ms = now_ms;
                    exit_click_pending = true;
                }
            }
        }

        if (exit_click_pending &&
            now_ms - first_exit_click_ms > SELF_TEST_DOUBLE_PRESS_MS) {
            exit_click_pending = false;
        }

        if (state != CAR_STATE_NORMAL) {
            atomic_set(&selected_indicator, INDICATOR_OFF);
            turn_armed = false;

            if (state != CAR_STATE_SELF_TEST) {
                exit_click_pending = false;
            }

            k_sleep(K_MSEC(BLINKERS_CHECK_MS));
            continue;
        }

        if (right_press) {
            select_indicator(INDICATOR_RIGHT, &turn_armed);
        }

        if (left_press) {
            select_indicator(INDICATOR_LEFT, &turn_armed);
        }

        enum indicator_mode selected = blinkers_get_mode();

        if (selected == INDICATOR_RIGHT) {
            if (command.steering >= TURN_THRESHOLD) {
                turn_armed = true;
            } else if (turn_armed &&
                       command.steering <= RETURN_THRESHOLD) {
                atomic_set(&selected_indicator, INDICATOR_OFF);
                turn_armed = false;
            }
        } else if (selected == INDICATOR_LEFT) {
            if (command.steering <= -TURN_THRESHOLD) {
                turn_armed = true;
            } else if (turn_armed &&
                       command.steering >= -RETURN_THRESHOLD) {
                atomic_set(&selected_indicator, INDICATOR_OFF);
                turn_armed = false;
            }
        }

        k_sleep(K_MSEC(BLINKERS_CHECK_MS));
    }
}

void blinkers_start(void)
{
    k_thread_create(&blinkers_thread_data, blinkers_stack,
                    K_THREAD_STACK_SIZEOF(blinkers_stack),
                    blinkers_thread, NULL, NULL, NULL,
                    BLINKERS_PRIORITY, 0, K_NO_WAIT);
}