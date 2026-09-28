#ifndef LED_H
#define LED_H

enum led_position {
    LED_FRONT_LEFT,
    LED_FRONT_RIGHT,
    LED_BACK_LEFT,
    LED_BACK_RIGHT,
};

int led_init(void);

void led_hazards_start(void);
void led_right_start(void);
void led_left_start(void);

void led_show_one(enum led_position position);
void led_all_off(void);

#endif