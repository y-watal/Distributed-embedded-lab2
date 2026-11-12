/* Zephyr-specific CTRL helpers. Wire definitions live in shared/protocol.h. */
#ifndef CATALOG_H
#define CATALOG_H

#include <zephyr/sys/byteorder.h>
#include "protocol.h"
#include "raft_wire.h"

/* Fills bytes 1-7; call after byte 0 (sequence) is set, since the CRC covers it. */
static inline void ctrl_encode(const struct ctrl_cmd *cmd, uint8_t data[FRAME_DLC])
{
	sys_put_le16((uint16_t)cmd->steer, &data[1]);
	data[3] = cmd->throttle;
	data[4] = cmd->brake;
	data[5] = cmd->buttons;
	data[6] = crc8_j1850(data, CTRL_CRC_LEN);
	data[7] = 0;
}

static inline void ctrl_decode(const uint8_t data[FRAME_DLC], struct ctrl_cmd *cmd)
{
	cmd->steer = (int16_t)sys_get_le16(&data[1]);
	cmd->throttle = data[3];
	cmd->brake = data[4];
	cmd->buttons = data[5];
}

/*
 * Validates a received CTRL payload: FAULT_NONE if usable, else the Lab 1
 * fault code to report (CTRL_MALFORMED for a bad CRC or reserved bits,
 * CTRL_RANGE for an out-of-range value). Every uint8 pedal value is valid.
 */
static inline enum fault_code ctrl_check(const uint8_t data[FRAME_DLC], struct ctrl_cmd *cmd)
{
	if (crc8_j1850(data, CTRL_CRC_LEN) != data[6] || data[7] != 0) {
		return FAULT_CTRL_MALFORMED;
	}
	ctrl_decode(data, cmd);
	if (cmd->steer < CTRL_STEER_MIN || cmd->steer > CTRL_STEER_MAX ||
	    (cmd->buttons & ~CTRL_BTN_MASK) != 0) {
		return FAULT_CTRL_RANGE;
	}
	return FAULT_NONE;
}

#endif /* CATALOG_H */
