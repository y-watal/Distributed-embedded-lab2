/* Node identity and hardware: the only shared-name file that differs per node. */
#ifndef NODE_CONFIG_H
#define NODE_CONFIG_H

#include "catalog.h"

#define NODE_SELF     NODE_N2
#define NODE_NAME     "N2"
#define RELAY_ENABLED 1 /* N2 forwards every other node's frames to the other bus */

/* Current sensors: {ADC1 channel, polarity, name}. Servo: PC1 = ADC1_IN11 (Lab 2). */
#define CURRENT_CHANNELS {{11, 1, "servo"}}

#endif /* NODE_CONFIG_H */
