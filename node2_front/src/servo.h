/* Steering servo on TIM3_CH1 / PB4 (SRV test point), from Lab 2. */
#ifndef SERVO_H
#define SERVO_H

#define SERVO_LEFT_DEG   0
#define SERVO_CENTER_DEG 90
#define SERVO_RIGHT_DEG  180

int servo_init(void); /* centers the servo */
int servo_set_angle(int angle_deg);

#endif /* SERVO_H */
