#ifndef CURRENT_SENSOR_H
#define CURRENT_SENSOR_H

#include <stdint.h>

struct current_sensor_readings {
    int16_t left_raw;
    int16_t right_raw;
    int16_t servo_raw;

    int32_t left_ma;
    int32_t right_ma;
    int32_t servo_ma;
};

int current_sensor_init(void);
int current_sensor_read(struct current_sensor_readings *readings);

#endif