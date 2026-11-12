#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "current_sensor.h"

#define SENSOR_COUNT 3

#define ADC_REFERENCE_MV 3300
#define ADC_COUNTS 4096
#define DIVIDER_FACTOR 2
#define SENSITIVITY_MV_PER_A 185

#define ZERO_SAMPLES 128
#define READ_SAMPLES 16
#define RAW_SCALE 256

enum {
    LEFT_SENSOR,
    RIGHT_SENSOR,
    SERVO_SENSOR
};

/* Left motor: PB0; right motor: PA4; servo: PC1. */
static const uint8_t channels[SENSOR_COUNT] = {8, 4, 11};

/* Positive current for the wiring directions used in our tests */
static const int polarity[SENSOR_COUNT] = {-1, 1, 1};

static const struct device *const adc_dev =
    DEVICE_DT_GET(DT_NODELABEL(adc1));

static int32_t zero_scaled[SENSOR_COUNT];
static bool calibrated;

static int read_channel(uint8_t channel, int16_t *sample)
{
    struct adc_sequence sequence = {
        .channels = BIT(channel),
        .buffer = sample,
        .buffer_size = sizeof(*sample),
        .resolution = 12,
    };

    return adc_read(adc_dev, &sequence);
}

static int32_t convert_to_ma(int32_t raw_scaled, int sensor)
{
    int64_t delta = (int64_t)raw_scaled - zero_scaled[sensor];

    int64_t numerator = delta * ADC_REFERENCE_MV *
        DIVIDER_FACTOR * 1000 * polarity[sensor];

    int64_t denominator = (int64_t)RAW_SCALE *
        ADC_COUNTS * SENSITIVITY_MV_PER_A;

    return (int32_t)(numerator / denominator);
}

int current_sensor_init(void)
{
    int ret;
    int16_t sample;
    int32_t sums[SENSOR_COUNT] = {0};

    calibrated = false;

    if (!device_is_ready(adc_dev)) {
        return -ENODEV;
    }

    for (int i = 0; i < SENSOR_COUNT; i++) {
        struct adc_channel_cfg config = {
            .gain = ADC_GAIN_1,
            .reference = ADC_REF_INTERNAL,
            .acquisition_time = ADC_ACQ_TIME(ADC_ACQ_TIME_TICKS, 480),
            .channel_id = channels[i],
        };

        ret = adc_channel_setup(adc_dev, &config);
        if (ret != 0) {
            return ret;
        }
    }

    printk("Calibrating all sensors with zero load current\n");
    k_msleep(100);

    for (int n = 0; n < ZERO_SAMPLES; n++) {
        for (int i = 0; i < SENSOR_COUNT; i++) {
            ret = read_channel(channels[i], &sample);
            if (ret != 0) {
                return ret;
            }

            sums[i] += sample;
        }

        k_msleep(5);
    }

    for (int i = 0; i < SENSOR_COUNT; i++) {
        zero_scaled[i] = sums[i] * RAW_SCALE / ZERO_SAMPLES;
    }

    calibrated = true;

    printk("Zero counts: left=%d right=%d servo=%d\n",
           (int)(zero_scaled[LEFT_SENSOR] / RAW_SCALE),
           (int)(zero_scaled[RIGHT_SENSOR] / RAW_SCALE),
           (int)(zero_scaled[SERVO_SENSOR] / RAW_SCALE));

    return 0;
}

int current_sensor_read(struct current_sensor_readings *readings)
{
    int ret;
    int16_t sample;
    int32_t sums[SENSOR_COUNT] = {0};
    int32_t scaled[SENSOR_COUNT];

    if (readings == NULL) {
        return -EINVAL;
    }

    if (!calibrated) {
        return -EAGAIN;
    }

    for (int n = 0; n < READ_SAMPLES; n++) {
        for (int i = 0; i < SENSOR_COUNT; i++) {
            ret = read_channel(channels[i], &sample);
            if (ret != 0) {
                return ret;
            }

            sums[i] += sample;
        }
    }

    for (int i = 0; i < SENSOR_COUNT; i++) {
        scaled[i] = sums[i] * RAW_SCALE / READ_SAMPLES;
    }

    readings->left_raw = sums[LEFT_SENSOR] / READ_SAMPLES;
    readings->right_raw = sums[RIGHT_SENSOR] / READ_SAMPLES;
    readings->servo_raw = sums[SERVO_SENSOR] / READ_SAMPLES;

    readings->left_ma = convert_to_ma(scaled[LEFT_SENSOR], LEFT_SENSOR);
    readings->right_ma = convert_to_ma(scaled[RIGHT_SENSOR], RIGHT_SENSOR);
    readings->servo_ma = convert_to_ma(scaled[SERVO_SENSOR], SERVO_SENSOR);

    return 0;
}
