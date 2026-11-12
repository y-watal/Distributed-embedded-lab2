#ifndef MOTOR_CONTROL_H
#define MOTOR_CONTROL_H

#include <stdint.h>

struct motor_telemetry {
    int32_t left_speed_mm_s;
    int32_t right_speed_mm_s;
    uint16_t left_duty_permille;
    uint16_t right_duty_permille;
};

void motor_control_start(void);
void motor_control_get_telemetry(struct motor_telemetry *telemetry);

#endif
