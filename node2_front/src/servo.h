/* Steering servo on TIM3_CH1 / PB4, from Lab 2. servo_set_angle() toggles the
 * SRV test point (D14 / PB9) on every successful set. */
#ifndef SERVO_H
#define SERVO_H

#define SERVO_LEFT_DEG   0
#define SERVO_CENTER_DEG 90
#define SERVO_RIGHT_DEG  180

int servo_init(void); /* centers the servo */
int servo_set_angle(int angle_deg);

#endif /* SERVO_H */
