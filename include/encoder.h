#ifndef ENCODER_H
#define ENCODER_H

#include <stdint.h>

struct encoder_readings {
    int64_t left_count;
    int64_t right_count;

    /* 1000 mm/s = 1 m/s */
    int32_t left_velocity_mm_s;
    int32_t right_velocity_mm_s;
};

int encoder_init(void);
int encoder_read(struct encoder_readings *readings);

#endif