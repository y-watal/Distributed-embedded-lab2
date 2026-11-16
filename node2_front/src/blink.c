#include "blink.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include "catalog.h"
#include "blink_phase.h"
#include "evlog.h"
#include "gtime.h"
#include "hb.h"
#include "lab3_config.h"
#include "node_config.h"
#include "tp.h"
#include "wcet.h"
#include "zone.h"

#define BLINK_STACK_SIZE 1024
#define BLINK_POLL_US    10000 /* re-check zone, peers and BLINK age at least this often */

static atomic_t last_bits;      /* byte 1 of the last BLINK */
static atomic_t last_rx_ms;     /* 0 = never */
static atomic_t local_hazard;   /* half period of a local hazard, 0 = none */
static atomic_t lamp_half_ms;   /* half period now in use, 0 = lamps off */
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

/* Half period for a cockpit BLINK mode; 0 = off. */
static uint32_t mode_half_ms(uint8_t mode)
{
	if (mode == (BLINK_LEFT | BLINK_RIGHT)) {
		return BLINK_HAZARD_HALF_MS;
	}
	return mode ? BLINK_TURN_HALF_MS : 0;
}

static void blink_thread(void *p1, void *p2, void *p3)
{
	uint8_t logged_mode = 0;
	uint32_t wait_us = 0;
	struct wcet_job job = WCET_JOB(WCET_BLINK);

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		/* A frame wakes us at once; otherwise the next lamp edge, or 10 ms. */
		wcet_end(&job);
		k_sem_take(&blink_new, K_USEC(wait_us));
		wcet_begin(&job);

		uint32_t now = k_uptime_get_32();
		uint32_t rx = (uint32_t)atomic_get(&last_rx_ms);
		/* Cockpit down: the vehicle's failure mode, hazards at 4 Hz. */
		bool failure = !hb_peer_alive(NODE_N1);
		bool hazard_here = zone_state() == ZONE_ISOLATED || failure ||
				   rx == 0 || now - rx > BLINK_STALE_MS;
		uint32_t half_ms;
		bool left, right;

		if (hazard_here) {
			half_ms = failure ? BLINK_FAILURE_HALF_MS : BLINK_HAZARD_HALF_MS;
			if ((uint32_t)atomic_get(&local_hazard) != half_ms) {
				if (zone_state() == ZONE_ISOLATED) {
					evlog("local hazard ON (%u Hz): zone ISOLATED", 500u / half_ms);
				} else if (failure) {
					evlog("local hazard ON (%u Hz): N1 heartbeat lost, failure mode",
					      500u / half_ms);
				} else if (rx == 0) {
					evlog("local hazard ON (%u Hz): no BLINK received yet",
					      500u / half_ms);
				} else {
					evlog("local hazard ON (%u Hz): no BLINK for %u ms",
					      500u / half_ms, now - rx);
				}
				atomic_set(&local_hazard, (atomic_val_t)half_ms);
			}
			left = right = true;
		} else {
			if (atomic_get(&local_hazard)) {
				evlog("local hazard off");
			}
			atomic_set(&local_hazard, 0);

			uint8_t mode = (uint8_t)atomic_get(&last_bits) & (BLINK_LEFT | BLINK_RIGHT);

			if (mode != logged_mode) {
				static const char *const names[] = {"off", "LEFT", "RIGHT", "HAZARD"};

				evlog("cockpit BLINK %s -> %s", names[logged_mode], names[mode]);
				logged_mode = mode;
			}
			half_ms = mode_half_ms(mode);
			left = mode & BLINK_LEFT;
			right = mode & BLINK_RIGHT;
		}
		atomic_set(&lamp_half_ms, (atomic_val_t)half_ms);

		/* Phase from global time, not from the BLINK frame: front and rear
		 * switch at the same global instant (shared/blink_phase.h). */
		uint64_t g = global_gettime();
		bool on = blink_phase_on(g, half_ms);

		set_lamps(on && left, on && right);

		uint64_t to_edge = half_ms ? blink_us_to_edge(g, half_ms) : BLINK_POLL_US;

		wait_us = (uint32_t)(to_edge < BLINK_POLL_US ? to_edge : BLINK_POLL_US);
	}
}

const char *blink_describe(void)
{
	static const char *const names[] = {"off", "LEFT", "RIGHT", "HAZARD"};
	static char buf[40];
	uint8_t bits = (uint8_t)atomic_get(&last_bits) & (BLINK_LEFT | BLINK_RIGHT);
	bool local = atomic_get(&local_hazard) != 0;
	uint32_t half = (uint32_t)atomic_get(&lamp_half_ms);
	int l = (int)atomic_get(&lamps);

	snprintk(buf, sizeof(buf), "%s%s %uHz, lamps %s", local ? "HAZARD" : names[bits],
		 local ? " (local)" : "", half ? 500u / half : 0u,
		 l == 3 ? "both on" : (l == 1 ? "L on" : (l == 2 ? "R on" : "off")));
	return buf;
}

int blink_init(void)
{
	k_thread_create(&blink_data, blink_stack, K_THREAD_STACK_SIZEOF(blink_stack), blink_thread,
			NULL, NULL, NULL, PRIO_BLINK, 0, K_NO_WAIT);
	k_thread_name_set(&blink_data, "blink");
	return 0;
}
