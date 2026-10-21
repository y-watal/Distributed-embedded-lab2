/* Ported from Lab 2 src/drivers/current_sensor.c and src/tasks/current_sensing.c. */
#include "current_sensor.h"

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "lab3_config.h"
#include "node_config.h"

#define ADC_REFERENCE_MV     3300
#define ADC_COUNTS           4096
#define DIVIDER_FACTOR       2
#define SENSITIVITY_MV_PER_A 185

#define ZERO_SAMPLES 128
#define READ_SAMPLES 16
#define RAW_SCALE    256

#define CURRENT_STACK_SIZE 1024

static const struct current_channel channels[] = CURRENT_CHANNELS;
#define NUM_CHANNELS ARRAY_SIZE(channels)

static const struct device *const adc_dev = DEVICE_DT_GET(DT_NODELABEL(adc1));

static int32_t zero_scaled[NUM_CHANNELS];
static atomic_t latest_ma[NUM_CHANNELS];
static atomic_t have_reading;

K_THREAD_STACK_DEFINE(current_stack, CURRENT_STACK_SIZE);
static struct k_thread current_data;

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

static int32_t convert_to_ma(int32_t raw_scaled, int i)
{
	int64_t delta = (int64_t)raw_scaled - zero_scaled[i];
	int64_t numerator = delta * ADC_REFERENCE_MV * DIVIDER_FACTOR * 1000 * channels[i].polarity;
	int64_t denominator = (int64_t)RAW_SCALE * ADC_COUNTS * SENSITIVITY_MV_PER_A;

	return (int32_t)(numerator / denominator);
}

static void current_thread(void *p1, void *p2, void *p3)
{
	int64_t next = k_uptime_get();

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		int32_t sums[NUM_CHANNELS] = {0};
		int16_t sample;
		int ret = 0;

		for (int n = 0; n < READ_SAMPLES && ret == 0; n++) {
			for (size_t i = 0; i < NUM_CHANNELS && ret == 0; i++) {
				ret = read_channel(channels[i].adc_channel, &sample);
				sums[i] += sample;
			}
		}
		if (ret == 0) {
			for (size_t i = 0; i < NUM_CHANNELS; i++) {
				atomic_set(&latest_ma[i],
					   convert_to_ma(sums[i] * RAW_SCALE / READ_SAMPLES, i));
			}
			atomic_set(&have_reading, 1);
		}

		next += CURRENT_PERIOD_MS;
		k_sleep(K_TIMEOUT_ABS_MS(next));
	}
}

int current_sensor_init(void)
{
	int32_t sums[NUM_CHANNELS] = {0};
	int16_t sample;
	int ret;

	if (!device_is_ready(adc_dev)) {
		return -ENODEV;
	}

	for (size_t i = 0; i < NUM_CHANNELS; i++) {
		struct adc_channel_cfg config = {
			.gain = ADC_GAIN_1,
			.reference = ADC_REF_INTERNAL,
			.acquisition_time = ADC_ACQ_TIME(ADC_ACQ_TIME_TICKS, 480),
			.channel_id = channels[i].adc_channel,
		};

		ret = adc_channel_setup(adc_dev, &config);
		if (ret != 0) {
			return ret;
		}
	}

	printk("Calibrating current sensors (no load current)\n");
	k_msleep(100);
	for (int n = 0; n < ZERO_SAMPLES; n++) {
		for (size_t i = 0; i < NUM_CHANNELS; i++) {
			ret = read_channel(channels[i].adc_channel, &sample);
			if (ret != 0) {
				return ret;
			}
			sums[i] += sample;
		}
		k_msleep(5);
	}
	printk("Zero counts:");
	for (size_t i = 0; i < NUM_CHANNELS; i++) {
		zero_scaled[i] = sums[i] * RAW_SCALE / ZERO_SAMPLES;
		printk(" %s=%d", channels[i].name, (int)(zero_scaled[i] / RAW_SCALE));
	}
	printk("\n");

	k_thread_create(&current_data, current_stack, K_THREAD_STACK_SIZEOF(current_stack),
			current_thread, NULL, NULL, NULL, PRIO_CURRENT, 0, K_NO_WAIT);
	k_thread_name_set(&current_data, "current");
	return 0;
}

int current_sensor_count(void)
{
	return NUM_CHANNELS;
}

const char *current_sensor_name(int i)
{
	return channels[i].name;
}

bool current_sensor_get_ma(int i, int32_t *ma)
{
	*ma = (int32_t)atomic_get(&latest_ma[i]);
	return atomic_get(&have_reading) != 0;
}
