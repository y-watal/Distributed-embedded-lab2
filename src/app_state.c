#include "app_state.h"

#include <zephyr/kernel.h>

K_MUTEX_DEFINE(command_mutex);
K_SEM_DEFINE(command_available, 0, 1);

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

    k_sem_give(&command_available);
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

void app_state_wait_for_command(void)
{
    k_sem_take(&command_available, K_FOREVER);
}

bool app_state_wait_for_command_ms(int32_t timeout_ms)
{
    return k_sem_take(&command_available, K_MSEC(timeout_ms)) == 0;
}