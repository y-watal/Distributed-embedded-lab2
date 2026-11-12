/* Pi-specific CTRL helpers and catalog slots. Wire definitions are shared. */
#ifndef CATALOG_PI_H
#define CATALOG_PI_H

#include "protocol.h"
#include "raft_wire.h"

/* Fills bytes 1-7; data[0] (sequence) must already be set, the CRC covers it. */
static inline void ctrl_encode(const struct ctrl_cmd *cmd, uint8_t data[FRAME_DLC])
{
	uint16_t s = (uint16_t)cmd->steer;

	data[1] = (uint8_t)(s & 0xFF);
	data[2] = (uint8_t)(s >> 8);
	data[3] = cmd->throttle;
	data[4] = cmd->brake;
	data[5] = cmd->buttons;
	data[6] = crc8_j1850(data, CTRL_CRC_LEN);
	data[7] = 0;
}

static inline int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

/*
 * Lab 2 raw wheel values -> CTRL fields (same mapping as catalog.py's
 * ctrl_from_lab2). Steering outside Lab 2's range is sent as
 * CTRL_STEER_INVALID so the receivers reject it; pedals have no
 * out-of-range encoding in a uint8 and are clamped.
 */
static inline void ctrl_from_lab2(int32_t steer, int32_t throttle, int32_t brake, bool btn4,
				  bool btn5, bool btn10, struct ctrl_cmd *cmd)
{
	cmd->steer = (steer >= CTRL_STEER_MIN && steer <= CTRL_STEER_MAX) ? (int16_t)steer
									    : CTRL_STEER_INVALID;
	cmd->throttle = (uint8_t)(clamp_i32(throttle, 0, 32767) >> CTRL_PEDAL_SHIFT);
	cmd->brake = (uint8_t)(clamp_i32(brake, 0, 32767) >> CTRL_PEDAL_SHIFT);
	cmd->buttons = (uint8_t)((btn4 ? CTRL_BTN4 : 0) | (btn5 ? CTRL_BTN5 : 0) |
				 (btn10 ? CTRL_BTN10 : 0));
}

/* Catalog IDs in a fixed order, for per-ID counters and sequence numbers. */
static const uint16_t catalog_ids[] = {
	ID_CTRL, ID_FAULT(0), ID_FAULT(1), ID_FAULT(2), ID_HB(0), ID_HB(1), ID_HB(2), ID_BLINK, ID_FORCE,
};
#define NUM_SLOTS (sizeof(catalog_ids) / sizeof(catalog_ids[0]))

static inline int catalog_slot(uint32_t id)
{
	for (size_t i = 0; i < NUM_SLOTS; i++) {
		if (catalog_ids[i] == id) {
			return (int)i;
		}
	}
	return -1;
}

#endif /* CATALOG_PI_H */
