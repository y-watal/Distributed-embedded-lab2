#pragma once

#include <stdbool.h>

// Initialize the H-bridge pins with both motors disabled
int motor_init(void);

// Enable both motors at full drive or disable them to coast
int motor_set_enabled(bool enabled);
