#include "app_state.h"

#include <zephyr/kernel.h>

// One writer and multiple readers use the same short mutex
// Each reader copies the command, then releases the mutex
K_MUTEX_DEFINE(command_mutex);

static struct wheel_command latest_command;
static bool have_command;

void app_state_set_command(const struct wheel_command *command)
{
    if (command == NULL) {
        return;
    }

    k_mutex_lock(&command_mutex, K_FOREVER);

    latest_command = *command;
    latest_command.received_at_ms = k_uptime_get();
    have_command = true;

    k_mutex_unlock(&command_mutex);
}

bool app_state_get_command(struct wheel_command *command)
{
    if (command == NULL) {
        return false;
    }

    k_mutex_lock(&command_mutex, K_FOREVER);

    bool available = have_command;

    if (available) {
        *command = latest_command;
    }

    k_mutex_unlock(&command_mutex);

    return available;
}