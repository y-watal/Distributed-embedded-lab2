#include "zone.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "canbus.h"
#include "ctrl.h"
#include "hb.h"
#include "lab3_config.h"
#include "node_config.h"
#include "tp.h"

static struct k_spinlock lock; /* everything below */

static bool test_fault, exit_pending;
static uint32_t first_exit_ms;
static uint8_t ctrl_fault;
static uint32_t ctrl_fault_ms;
static bool peer_fault[NUM_NODES];
static uint8_t peer_fault_code[NUM_NODES];
static uint32_t peer_fault_ms[NUM_NODES];
static uint8_t peer_state[NUM_NODES]; /* from their heartbeats */
static uint8_t local_fault = FAULT_NONE, last_fault = FAULT_NONE;
static enum zone_state state = ZONE_BOOT;
static bool booted;
static bool prev_alive[NUM_NODES];

#define FAULT_QUEUE 4
static struct {
	uint8_t code, detail;
} fault_q[FAULT_QUEUE];
static int fault_q_len;
static uint32_t last_fault_sent_ms;

/* zone_update runs from HB_MON and from the test button thread. */
K_MUTEX_DEFINE(update_lock);

const char *zone_state_name(enum zone_state s)
{
	static const char *const names[] = {"BOOT", "SYS_ERROR", "NORMAL", "ISOLATED"};

	return s <= ZONE_ISOLATED ? names[s] : "?";
}

static void queue_fault_locked(uint8_t code, uint8_t detail)
{
	if (fault_q_len < FAULT_QUEUE) {
		fault_q[fault_q_len].code = code;
		fault_q[fault_q_len].detail = detail;
		fault_q_len++;
	}
}

void zone_update(void)
{
	uint32_t now = k_uptime_get_32();
	bool fresh = ctrl_fresh(); /* own lock; take it before ours */
	bool alive[NUM_NODES] = {0};
	bool send = false;
	uint8_t code = 0, detail = 0;

	for (int n = 0; n < NUM_NODES; n++) {
		alive[n] = n != NODE_SELF && hb_peer_alive(n);
	}

	k_mutex_lock(&update_lock, K_FOREVER);
	k_spinlock_key_t key = k_spin_lock(&lock);

	bool all_alive = true, none_alive = true, peer_bad = false;

	for (int n = 0; n < NUM_NODES; n++) {
		if (n == NODE_SELF) {
			continue;
		}
		all_alive = all_alive && alive[n];
		none_alive = none_alive && !alive[n];
		if (alive[n] && (peer_fault[n] || peer_state[n] == ZONE_BOOT ||
				 peer_state[n] == ZONE_ISOLATED)) {
			peer_bad = true;
		}
		if (booted && prev_alive[n] && !alive[n]) {
			queue_fault_locked(FAULT_PEER_LOST, (uint8_t)n); /* informational */
		}
		prev_alive[n] = alive[n];
	}

	uint8_t lf = test_fault ? FAULT_TEST_BUTTON
				: (ctrl_fault ? ctrl_fault
					      : ((booted && !fresh) ? FAULT_CTRL_TIMEOUT : FAULT_NONE));

	if (lf != local_fault) {
		if (lf != FAULT_NONE && booted) {
			queue_fault_locked(lf, (lf == FAULT_CTRL_RANGE || lf == FAULT_CTRL_MALFORMED)
						       ? (uint8_t)(ID_CTRL & 0xFF)
						       : 0);
		}
		if (lf != FAULT_NONE) {
			last_fault = lf;
		}
		local_fault = lf;
	}

	if (!booted && all_alive && fresh) {
		booted = true;
	}
	state = !booted	     ? ZONE_BOOT
		: none_alive ? ZONE_ISOLATED
		: (local_fault || !all_alive || peer_bad) ? ZONE_SYS_ERROR
							  : ZONE_NORMAL;

	if (fault_q_len > 0 && now - last_fault_sent_ms >= FAULT_MIN_GAP_MS) {
		code = fault_q[0].code;
		detail = fault_q[0].detail;
		for (int i = 1; i < fault_q_len; i++) {
			fault_q[i - 1] = fault_q[i];
		}
		fault_q_len--;
		last_fault_sent_ms = now;
		send = true;
	}
	enum zone_state st = state;

	k_spin_unlock(&lock, key);

	tp_set(TP_ERR, st != ZONE_NORMAL);
	if (send) {
		uint8_t p[FRAME_DLC] = {0};

		p[1] = code;
		p[2] = detail;
		canbus_send(ID_FAULT(NODE_SELF), p, false);
	}
	k_mutex_unlock(&update_lock);
}

void zone_on_frame(const struct can_frame *frame)
{
	int sender = catalog_sender(frame->id);
	uint32_t now = k_uptime_get_32();

	if (sender < 0 || sender == NODE_SELF) {
		return;
	}

	k_spinlock_key_t key = k_spin_lock(&lock);

	if (frame->id == ID_HB(sender)) {
		peer_state[sender] = frame->data[1] & HB_B1_STATE_MASK;
		if (frame->data[1] & HB_B1_LOCAL_FAULT) {
			peer_fault[sender] = true;
			peer_fault_code[sender] = frame->data[3];
		} else if (now - peer_fault_ms[sender] > FAULT_CLEAR_HOLD_MS) {
			peer_fault[sender] = false;
		}
	} else if (frame->id == ID_FAULT(sender) && frame->data[1] != FAULT_PEER_LOST) {
		peer_fault[sender] = true;
		peer_fault_code[sender] = frame->data[1];
		peer_fault_ms[sender] = now;
	}
	k_spin_unlock(&lock, key);
}

void zone_ctrl_rejected(enum fault_code code)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	ctrl_fault = code;
	ctrl_fault_ms = k_uptime_get_32();
	k_spin_unlock(&lock, key);

	zone_update(); /* leave NORMAL and send the FAULT now (checkoff 10) */
}

void zone_ctrl_accepted(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	if (ctrl_fault && k_uptime_get_32() - ctrl_fault_ms >= CTRL_FAULT_HOLD_MS) {
		ctrl_fault = FAULT_NONE;
	}
	k_spin_unlock(&lock, key);
}

void zone_test_button(void)
{
	uint32_t now = k_uptime_get_32();
	k_spinlock_key_t key = k_spin_lock(&lock);

	if (!test_fault) {
		test_fault = true; /* single press: local fail */
		exit_pending = false;
	} else if (exit_pending && now - first_exit_ms <= DOUBLE_PRESS_MS) {
		test_fault = false; /* double press: recover */
		exit_pending = false;
	} else {
		first_exit_ms = now;
		exit_pending = true;
	}
	k_spin_unlock(&lock, key);

	zone_update(); /* act now, not at the next HB_MON tick */
	ctrl_kick();   /* and wake the actuator task (R2.5: local fail within 10 ms) */
}

void zone_note_fault(enum fault_code code)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	last_fault = code;
	k_spin_unlock(&lock, key);
}

enum zone_state zone_state(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	enum zone_state s = state;

	k_spin_unlock(&lock, key);
	return s;
}

uint8_t zone_local_fault(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	uint8_t f = local_fault;

	k_spin_unlock(&lock, key);
	return f;
}

uint8_t zone_last_fault(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	uint8_t f = local_fault ? local_fault : last_fault;

	k_spin_unlock(&lock, key);
	return f;
}

bool zone_peer_fault(int node, uint8_t *code)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool f = peer_fault[node];

	*code = peer_fault_code[node];
	k_spin_unlock(&lock, key);
	return f;
}

enum zone_state zone_peer_state(int node)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	enum zone_state s = (enum zone_state)peer_state[node];

	k_spin_unlock(&lock, key);
	return s;
}
