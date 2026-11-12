/* Wheel encoders from Lab 2: left on TIM1 (PA8/PA9), right on TIM2 (PA15/PB3). */
#ifndef ENCODER_H
#define ENCODER_H

#include <stdint.h>

struct encoder_readings {
	int64_t left_count;
	int64_t right_count;
	int32_t left_velocity_mm_s; /* 1000 mm/s = 1 m/s */
	int32_t right_velocity_mm_s;
};

int encoder_init(void);

/* Velocity is averaged over the time since the previous call. PID thread only. */
int encoder_read(struct encoder_readings *readings);

#endif /* ENCODER_H */
