#include "encoder.h"

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>

#include <stm32_ll_tim.h>

#define COUNTS_PER_WHEEL_REV 1316
#define WHEEL_CIRCUMFERENCE_UM 235619  /* pi * 75 mm */

#define LEFT_SIGN  1
#define RIGHT_SIGN 1

static const struct device *const left_device =
    DEVICE_DT_GET(DT_NODELABEL(left_encoder));

static const struct device *const right_device =
    DEVICE_DT_GET(DT_NODELABEL(right_encoder));

struct encoder_state {
    TIM_TypeDef *timer;
    uint32_t previous;
    int64_t total;
};

static struct encoder_state left = {
    .timer = TIM1,
};

static struct encoder_state right = {
    .timer = TIM2,
};

static int64_t update_count(struct encoder_state *encoder)
{
    uint32_t current = LL_TIM_GetCounter(encoder->timer);
    int64_t modulus = (int64_t)LL_TIM_GetAutoReload(encoder->timer) + 1;
    int64_t delta = (int64_t)current - encoder->previous;

    /* Account for the timer rolling over between reads */
    if (delta > modulus / 2) {
        delta -= modulus;
    } else if (delta < -(modulus / 2)) {
        delta += modulus;
    }

    encoder->previous = current;
    encoder->total += delta;

    return delta;
}

static int32_t velocity_mm_s(int64_t count_delta, int64_t elapsed_ms)
{
    if (elapsed_ms <= 0) {
        return 0;
    }

    /*
     * circumference is in micrometers
     * milliseconds-to-seconds and micrometers-to-millimeters cancel
     */
    return (int32_t)(
        count_delta * WHEEL_CIRCUMFERENCE_UM /
        (COUNTS_PER_WHEEL_REV * elapsed_ms)
    );
}

static int64_t previous_time_ms;
static bool initialized;

int encoder_init(void)
{
    if (!device_is_ready(left_device) || !device_is_ready(right_device)) {
        return -ENODEV;
    }

    left.previous = LL_TIM_GetCounter(left.timer);
    right.previous = LL_TIM_GetCounter(right.timer);
    left.total = 0;
    right.total = 0;

    previous_time_ms = k_uptime_get();
    initialized = true;

    return 0;
}

int encoder_read(struct encoder_readings *readings)
{
    if (readings == NULL || !initialized) {
        return -EINVAL;
    }

    int64_t now_ms = k_uptime_get();
    int64_t elapsed_ms = now_ms - previous_time_ms;

    int64_t left_delta = update_count(&left) * LEFT_SIGN;
    int64_t right_delta = update_count(&right) * RIGHT_SIGN;

    readings->left_count = left.total * LEFT_SIGN;
    readings->right_count = right.total * RIGHT_SIGN;
    readings->left_velocity_mm_s = velocity_mm_s(left_delta, elapsed_ms);
    readings->right_velocity_mm_s = velocity_mm_s(right_delta, elapsed_ms);

    previous_time_ms = now_ms;

    return 0;
}