/*
 * Shared dual-CAN layer.
 *
 *  - Both MCP2515 controllers, with receive filters for the catalog IDs only.
 *  - canbus_send(): queues one frame on bus A and bus B back to back with the
 *    same per-ID sequence number, bumped once.
 *  - One dispatch thread that drops duplicates by (ID, seq) - the A copy, the
 *    B copy and any relayed copy - before anything else sees the frame, and
 *    records when each node's heartbeat was last heard on each bus.
 */
#ifndef CANBUS_H
#define CANBUS_H

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/drivers/can.h>
#include <zephyr/sys/atomic.h>

#include "catalog.h"

enum { BUS_A = 0, BUS_B = 1, NUM_BUSES = 2 };

/* Called from the dispatch thread with the first copy of each frame. */
typedef void (*canbus_handler_t)(const struct can_frame *frame, int bus);

int canbus_init(void);

/* Receivers of first copies. The relay hook runs before the application handler. */
void canbus_set_relay_hook(canbus_handler_t hook);
void canbus_set_handler(canbus_handler_t handler);

/*
 * Sends payload (bytes 1-7 used; byte 0 is overwritten with the sequence
 * number) on both buses. Buses marked degraded are skipped unless
 * include_degraded is set (heartbeats keep going out so the link can recover).
 */
int canbus_send(uint32_t id, uint8_t payload[FRAME_DLC], bool include_degraded);

/* Queues a bit-identical copy of frame on one bus (the relay). */
int canbus_forward(int bus, const struct can_frame *frame);

/* Aborts every transmission still pending on bus (stale frames on a dead link). */
void canbus_flush(int bus);

bool canbus_bus_running(int bus);
void canbus_set_degraded(int bus, bool degraded);
bool canbus_is_degraded(int bus);

/*
 * k_uptime_get_32() of the last heartbeat from node on bus (any copy, relayed
 * or not); 0 = never heard.
 */
uint32_t canbus_last_hb_ms(int bus, int node);

/* True once if the controller went bus-off since the last call. */
bool canbus_take_busoff(int bus);

/* ---- statistics, read and cleared by the report thread ---- */
#define CANBUS_NUM_SLOTS 9 /* catalog IDs */

struct canbus_bus_stats {
	atomic_t rx[CANBUS_NUM_SLOTS]; /* every copy received on this bus */
	atomic_t tx_ok, tx_err, tx_full, tx_skipped;
};

struct canbus_stats {
	struct canbus_bus_stats bus[NUM_BUSES];
	atomic_t accepted[CANBUS_NUM_SLOTS]; /* first copies */
	atomic_t gaps[CANBUS_NUM_SLOTS];     /* sequence numbers never received */
	atomic_t duplicates, own_echo, unknown_id, rx_dropped;
};

extern struct canbus_stats canbus_stats;
extern const uint16_t canbus_slot_id[CANBUS_NUM_SLOTS];
const struct device *canbus_dev(int bus);

#endif /* CANBUS_H */
