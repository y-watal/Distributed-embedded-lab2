/* Node identity and hardware: the only shared-name file that differs per node. */
#ifndef NODE_CONFIG_H
#define NODE_CONFIG_H

#include "catalog.h"

#define NODE_SELF     NODE_N3
#define NODE_NAME     "N3"
#define RELAY_ENABLED 0 /* only N2 relays */

/*
 * Current sensors: {ADC1 channel, polarity, name}. Lab 2 wiring: left motor
 * PB0 = ADC1_IN8 (reads negative for forward), right motor PA4 = ADC1_IN4.
 */
#define CURRENT_CHANNELS {{8, -1, "L"}, {4, 1, "R"}}

/* BLINK_R: this zone's blinker lamps (test points / LEDs) */
#define BLINK_LAMP_LEFT  TP_RL
#define BLINK_LAMP_RIGHT TP_RR

#endif /* NODE_CONFIG_H */
