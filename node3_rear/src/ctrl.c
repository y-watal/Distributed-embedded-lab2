#include "ctrl.h"

#include "lab3_config.h"

struct ctrl_stats ctrl_stats;

static struct k_spinlock lock;
static struct ctrl_cmd latest;
static uint32_t latest_ms;
static bool have;

K_SEM_DEFINE(ctrl_new, 0, 1);

void ctrl_on_frame(const struct can_frame *frame)
{
	struct ctrl_cmd cmd;
	enum fault_code fault = ctrl_check(frame->data, &cmd);

	if (fault != FAULT_NONE) {
		/* Stage 4: error state + FAULT_Nx. For now, count and keep the old command. */
		atomic_inc(fault == FAULT_CTRL_MALFORMED ? &ctrl_stats.malformed
							 : &ctrl_stats.out_of_range);
		atomic_set(&ctrl_stats.last_fault, fault);
		return;
	}

	k_spinlock_key_t key = k_spin_lock(&lock);
	latest = cmd;
	latest_ms = k_uptime_get_32();
	have = true;
	k_spin_unlock(&lock, key);

	atomic_inc(&ctrl_stats.ok);
	k_sem_give(&ctrl_new);
}

bool ctrl_get(struct ctrl_cmd *cmd, uint32_t *age_ms)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool ok = have;

	if (ok) {
		*cmd = latest;
		*age_ms = k_uptime_get_32() - latest_ms;
	}
	k_spin_unlock(&lock, key);
	return ok;
}

bool ctrl_fresh(void)
{
	struct ctrl_cmd cmd;
	uint32_t age;

	return ctrl_get(&cmd, &age) && age < CTRL_TIMEOUT_MS;
}

bool ctrl_wait(k_timeout_t timeout)
{
	return k_sem_take(&ctrl_new, timeout) == 0;
}
