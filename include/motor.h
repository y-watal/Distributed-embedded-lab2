#pragma once

#include <stdbool.h>
#include <stdint.h>

int motor_init(void);
int motor_set_drive_allowed(bool allowed);
int motor_brake(void);

// 0 coasts; 1000 is full PWM duty
int motor_set_duties_permille(uint16_t left_duty, uint16_t right_duty);
int motor_set_duty_permille(uint16_t duty);
int motor_set_enabled(bool enabled);