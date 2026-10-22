#include "blink.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include "catalog.h"
#include "evlog.h"
#include "hb.h"
#include "lab3_config.h"
#include "node_config.h"
#include "tp.h"
#include "wcet.h"
#include "zone.h"

#define BLINK_STACK_SIZE 1024

static atomic_t last_bits;      /* byte 1 of the last BLINK */
static atomic_t last_rx_ms;     /* 0 = never */
static atomic_t local_hazard;
static atomic_t lamps;          /* bit 0 left, bit 1 right: what's lit now */

K_SEM_DEFINE(blink_new, 0, 1);
K_THREAD_STACK_DEFINE(blink_stack, BLINK_STACK_SIZE);
static struct k_thread blink_data;

void blink_on_frame(const struct can_frame *frame)
{
	uint32_t now = k_uptime_get_32();

	atomic_set(&last_bits, frame->data[1]);
	atomic_set(&last_rx_ms, now != 0 ? now : 1);
	k_sem_give(&blink_new);
}

static void set_lamps(bool left, bool right)
{
	tp_set(BLINK_LAMP_LEFT, left);
	tp_set(BLINK_LAMP_RIGHT, right);
	atomic_set(&lamps, (left ? 1 : 0) | (right ? 2 : 0));
}

static void blink_thread(void *p1, void *p2, void *p3)
{
	uint32_t next_local_toggle = 0;
	bool local_phase = false;
	uint8_t logged_mode = 0;
	struct wcet_job job = WCET_JOB(WCET_BLINK);

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		/* A frame wakes us at once; otherwise re-check every 10 ms. */
		wcet_end(&job);
		k_sem_take(&blink_new, K_MSEC(10));
		wcet_begin(&job);

		uint32_t now = k_uptime_get_32();
		uint32_t rx = (uint32_t)atomic_get(&last_rx_ms);
		bool hazard_here = zone_state() == ZONE_ISOLATED || !hb_peer_alive(NODE_N1) ||
				   rx == 0 || now - rx > BLINK_STALE_MS;

		if (hazard_here) {
			if (!atomic_get(&local_hazard)) {
				if (zone_state() == ZONE_ISOLATED) {
					evlog("local hazard ON: zone ISOLATED");
				} else if (!hb_peer_alive(NODE_N1)) {
					evlog("local hazard ON: N1 heartbeat lost");
				} else if (rx == 0) {
					evlog("local hazard ON: no BLINK received yet");
				} else {
					evlog("local hazard ON: no BLINK for %u ms", now - rx);
				}
				atomic_set(&local_hazard, 1);
				local_phase = true;
				next_local_toggle = now + BLINK_HALF_MS;
			} else if ((int32_t)(now - next_local_toggle) >= 0) {
				local_phase = !local_phase;
				next_local_toggle += BLINK_HALF_MS;
			}
			set_lamps(local_phase, local_phase);
			continue;
		}
		if (atomic_get(&local_hazard)) {
			evlog("local hazard off");
		}
		atomic_set(&local_hazard, 0);

		uint8_t bits = (uint8_t)atomic_get(&last_bits);
		uint8_t mode = bits & (BLINK_LEFT | BLINK_RIGHT);

		if (mode != logged_mode) {
			static const char *const names[] = {"off", "LEFT", "RIGHT", "HAZARD"};

			evlog("cockpit BLINK %s -> %s", names[logged_mode], names[mode]);
			logged_mode = mode;
		}
		bool on = bits & BLINK_PHASE;

		set_lamps(on && (bits & BLINK_LEFT), on && (bits & BLINK_RIGHT));
	}
}

const char *blink_describe(void)
{
	static const char *const names[] = {"off", "LEFT", "RIGHT", "HAZARD"};
	static char buf[32];
	uint8_t bits = (uint8_t)atomic_get(&last_bits) & (BLINK_LEFT | BLINK_RIGHT);
	bool local = atomic_get(&local_hazard);
	int l = (int)atomic_get(&lamps);

	snprintk(buf, sizeof(buf), "%s%s, lamps %s", local ? "HAZARD" : names[bits],
		 local ? " (local)" : "", l == 3 ? "both on" : (l == 1 ? "L on" : (l == 2 ? "R on" : "off")));
	return buf;
}

int blink_init(void)
{
	k_thread_create(&blink_data, blink_stack, K_THREAD_STACK_SIZEOF(blink_stack), blink_thread,
			NULL, NULL, NULL, PRIO_BLINK, 0, K_NO_WAIT);
	k_thread_name_set(&blink_data, "blink");
	return 0;
}
