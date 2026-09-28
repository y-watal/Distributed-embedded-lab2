#include "led.h"

#include <stdbool.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

/* Front left: D4 / PB5 */
#define FRONT_LEFT_PIN 5

/* Front right: D6 / PB10 */
#define FRONT_RIGHT_PIN 10

/* Back left: D9 / PC7 */
#define BACK_LEFT_PIN 7

/* Back right: D2 / PA10 */
#define BACK_RIGHT_PIN 10

#define HAZARD_HALF_PERIOD_MS 250
#define TURN_HALF_PERIOD_MS   500

enum blink_mode {
    BLINK_OFF,
    BLINK_HAZARDS,
    BLINK_RIGHT,
    BLINK_LEFT,
};

static const struct device *const gpioa =
    DEVICE_DT_GET(DT_NODELABEL(gpioa));

static const struct device *const gpiob =
    DEVICE_DT_GET(DT_NODELABEL(gpiob));

static const struct device *const gpioc =
    DEVICE_DT_GET(DT_NODELABEL(gpioc));

static atomic_t active_mode = ATOMIC_INIT(BLINK_OFF);
static struct k_work_sync blink_work_sync;
static bool phase_on;

static void blink_work_handler(struct k_work *work);

K_WORK_DELAYABLE_DEFINE(blink_work, blink_work_handler);

static void set_outputs(bool front_left, bool front_right,
                        bool back_left, bool back_right)
{
    gpio_pin_set(gpiob, FRONT_LEFT_PIN, front_left);
    gpio_pin_set(gpiob, FRONT_RIGHT_PIN, front_right);
    gpio_pin_set(gpioc, BACK_LEFT_PIN, back_left);
    gpio_pin_set(gpioa, BACK_RIGHT_PIN, back_right);
}

static void show_mode(enum blink_mode mode, bool on)
{
    switch (mode) {
    case BLINK_HAZARDS:
        set_outputs(on, on, on, on);
        break;

    case BLINK_RIGHT:
        set_outputs(false, on, false, on);
        break;

    case BLINK_LEFT:
        set_outputs(on, false, on, false);
        break;

    case BLINK_OFF:
    default:
        set_outputs(false, false, false, false);
        break;
    }
}

static int half_period_ms(enum blink_mode mode)
{
    return mode == BLINK_HAZARDS ?
           HAZARD_HALF_PERIOD_MS : TURN_HALF_PERIOD_MS;
}

static void blink_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    enum blink_mode mode = (enum blink_mode)atomic_get(&active_mode);

    if (mode == BLINK_OFF) {
        return;
    }

    phase_on = !phase_on;
    show_mode(mode, phase_on);

    if (atomic_get(&active_mode) == mode) {
        k_work_reschedule(&blink_work, K_MSEC(half_period_ms(mode)));
    }
}

static void stop_blinking(void)
{
    atomic_set(&active_mode, BLINK_OFF);

    /* Wait for any running LED update before selecting a new pattern */
    k_work_cancel_delayable_sync(&blink_work, &blink_work_sync);

    phase_on = false;
    show_mode(BLINK_OFF, false);
}

static void start_blinking(enum blink_mode mode)
{
    stop_blinking();

    atomic_set(&active_mode, mode);
    phase_on = true;
    show_mode(mode, true);

    k_work_reschedule(&blink_work, K_MSEC(half_period_ms(mode)));
}

int led_init(void)
{
    if (!device_is_ready(gpioa) ||
        !device_is_ready(gpiob) ||
        !device_is_ready(gpioc)) {
        return -1;
    }

    if (gpio_pin_configure(gpiob, FRONT_LEFT_PIN, GPIO_OUTPUT_INACTIVE) < 0 ||
        gpio_pin_configure(gpiob, FRONT_RIGHT_PIN, GPIO_OUTPUT_INACTIVE) < 0 ||
        gpio_pin_configure(gpioc, BACK_LEFT_PIN, GPIO_OUTPUT_INACTIVE) < 0 ||
        gpio_pin_configure(gpioa, BACK_RIGHT_PIN, GPIO_OUTPUT_INACTIVE) < 0) {
        return -1;
    }

    return 0;
}

void led_hazards_start(void)
{
    start_blinking(BLINK_HAZARDS);
}

void led_right_start(void)
{
    start_blinking(BLINK_RIGHT);
}

void led_left_start(void)
{
    start_blinking(BLINK_LEFT);
}

void led_show_one(enum led_position position)
{
    stop_blinking();

    switch (position) {
    case LED_FRONT_LEFT:
        set_outputs(true, false, false, false);
        break;

    case LED_FRONT_RIGHT:
        set_outputs(false, true, false, false);
        break;

    case LED_BACK_LEFT:
        set_outputs(false, false, true, false);
        break;

    case LED_BACK_RIGHT:
        set_outputs(false, false, false, true);
        break;
    }
}

void led_all_off(void)
{
    stop_blinking();
}