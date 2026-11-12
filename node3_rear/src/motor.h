/*
 * L298 H-bridge for the two drive motors, from Lab 2: ENA/ENB PWM on
 * TIM5_CH1/CH2 (PA0/PA1, 10 kHz), IN1-IN4 on PB12-PB15. PA0 is the PWM_OUT
 * test point and PB12 (IN1) the DIR_A test point. PWM_SET toggles on every
 * successful duty set (motor_set_duties_permille, 0 = coast) and brake set
 * (motor_brake, motor_set_drive_allowed(false)), changed or not.
 */
#ifndef MOTOR_H
#define MOTOR_H

#include <stdbool.h>
#include <stdint.h>

int motor_init(void); /* ends in brake, drive not allowed */
int motor_set_drive_allowed(bool allowed); /* false = dynamic brake */
int motor_brake(void);

/* 0 coasts; 1000 is full PWM duty. -EPERM unless drive is allowed. */
int motor_set_duties_permille(uint16_t left_duty, uint16_t right_duty);

#endif /* MOTOR_H */
