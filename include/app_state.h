#ifndef APP_STATE_H
#define APP_STATE_H

#include <stdbool.h>
#include <stdint.h>

struct wheel_command {
    uint32_t packet;
    int32_t steering;
    int32_t throttle;
    int32_t brake;
    uint8_t button4;
    uint8_t button5;
    uint8_t button10;
    int64_t received_at_ms;
};

void app_state_set_command(const struct wheel_command *command);
bool app_state_get_command(struct wheel_command *command);
void app_state_wait_for_command(void);
bool app_state_wait_for_command_ms(int32_t timeout_ms);

#endif