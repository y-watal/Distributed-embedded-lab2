#ifndef CURRENT_SENSING_H
#define CURRENT_SENSING_H

#include <stdbool.h>

#include "current_sensor.h"

/* Start only after current_sensor_init() succeeds. */
void current_sensing_start(void);
bool current_sensing_get_latest(struct current_sensor_readings *readings);

#endif
