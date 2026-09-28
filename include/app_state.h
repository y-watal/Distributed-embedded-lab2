#ifndef APP_STATE_H
#define APP_STATE_H

#include <stdbool.h>
#include <stdint.h>

// Raw values from one validated Pi command
// These are inputs, not servo angles or motor PWM values
struct wheel_command {
    uint32_t packet;
    int32_t steering;
    int32_t throttle;
    int32_t brake;
    uint8_t button4;
    uint8_t button5;
    uint8_t button10;

    // STM32 uptime when this complete, valid command was received
    int64_t received_at_ms;
};

// Called by the communication thread after validating a complete frame
void app_state_set_command(const struct wheel_command *command);

// Copies the latest complete command into *command
// Returns false until the first valid command has arrived
bool app_state_get_command(struct wheel_command *command);

#endif