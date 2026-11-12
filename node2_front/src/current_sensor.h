/*
 * ACS712-style current sensors on ADC1, from Lab 2 (185 mV/A behind a 2:1
 * divider, zero-current offset calibrated at boot). The channels a node reads
 * are listed in node_config.h (CURRENT_CHANNELS). The CURRENT thread samples
 * every CURRENT_PERIOD_MS at low priority.
 */
#ifndef CURRENT_SENSOR_H
#define CURRENT_SENSOR_H

#include <stdbool.h>
#include <stdint.h>

struct current_channel {
	uint8_t adc_channel;
	int8_t polarity; /* +1 / -1: positive current for our wiring direction */
	const char *name;
};

/*
 * Calibrates the zero offsets (blocks ~0.75 s; loads must carry no current),
 * then starts the CURRENT thread.
 */
int current_sensor_init(void);

int current_sensor_count(void);
const char *current_sensor_name(int i);

/* Latest reading of channel i in mA; false until the first sample. */
bool current_sensor_get_ma(int i, int32_t *ma);

#endif /* CURRENT_SENSOR_H */
