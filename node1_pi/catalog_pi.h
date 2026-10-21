/*
 * Lab 3 message catalog for the Pi's C code. Same values as
 * node2_front/src/catalog.h (Nucleos) and catalog.py (Pi, Python); full
 * table in docs/lab3_design.md.
 *
 * Byte 0 of every payload: low nibble = per-ID 4-bit sequence counter, high
 * nibble = flags (0). All frames are standard IDs, DLC 8, sent on both buses.
 */
#ifndef CATALOG_PI_H
#define CATALOG_PI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum node_idx { NODE_N1 = 0, NODE_N2 = 1, NODE_N3 = 2, NUM_NODES = 3 };

#define ID_CTRL     0x010
#define ID_FAULT(n) (0x020 + (n))
#define ID_HB(n)    (0x100 + (n))
#define ID_BLINK    0x200
#define ID_FORCE    0x300

#define FRAME_DLC 8
#define SEQ_MASK  0x0F

/* HB_Nx (Lab 1 HB_x layout) */
enum zone_state { ZONE_BOOT = 0, ZONE_SYS_ERROR = 1, ZONE_NORMAL = 2, ZONE_ISOLATED = 3 };
#define HB_B1_STATE_MASK  0x0F
#define HB_B1_LOCAL_FAULT (1u << 4)
#define HB_B2_A_DEGRADED  (1u << 0)
#define HB_B2_B_DEGRADED  (1u << 1)
#define HB_B2_A_BUSOFF    (1u << 2)
#define HB_B2_B_BUSOFF    (1u << 3)
#define HB_B2_RELAY       (1u << 4)

/* FAULT_Nx (Lab 1 ERR_NOTIFY_x codes); byte 1 code, byte 2 detail */
enum fault_code {
	FAULT_NONE = 0x00,
	FAULT_TEST_BUTTON = 0x01,
	FAULT_UDP_TIMEOUT = 0x02,
	FAULT_ISOLATED = 0x03,
	FAULT_CTRL_RANGE = 0x04,
	FAULT_CTRL_MALFORMED = 0x05,
	FAULT_CTRL_TIMEOUT = 0x06,
	FAULT_MOTOR_OPEN = 0x07,
	FAULT_MOTOR_OVERLOAD = 0x08,
	FAULT_WATCHDOG_RESET = 0x09,
	FAULT_PEER_LOST = 0x0A,
};

/* BLINK byte 1 (Lab 1 BLINK_SYNC layout) */
#define BLINK_LEFT  (1u << 0)
#define BLINK_RIGHT (1u << 1) /* both = hazard */
#define BLINK_PHASE (1u << 2) /* 1 = active lamps on */

/*
 * CTRL (handout template): bytes 1-2 steer int16 LE, 3 throttle u8, 4 brake
 * u8, 5 buttons, 6 CRC-8/SAE-J1850 over bytes 0-5, 7 reserved 0.
 */
struct ctrl_cmd {
	int16_t steer;
	uint8_t throttle;
	uint8_t brake;
	uint8_t buttons;
};
#define CTRL_STEER_MIN     (-32767)
#define CTRL_STEER_MAX     32766
#define CTRL_STEER_INVALID (-32768) /* sent for an out-of-range wheel value */
#define CTRL_PEDAL_SHIFT   7
#define CTRL_BTN4          (1u << 0)
#define CTRL_BTN5          (1u << 1)
#define CTRL_BTN10         (1u << 2)

static inline uint8_t crc8_j1850(const uint8_t *data, size_t len)
{
	uint8_t crc = 0xFF;

	for (size_t i = 0; i < len; i++) {
		crc ^= data[i];
		for (int b = 0; b < 8; b++) {
			crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x1D) : (uint8_t)(crc << 1);
		}
	}
	return crc ^ 0xFF;
}

/* Fills bytes 1-7; data[0] (sequence) must already be set, the CRC covers it. */
static inline void ctrl_encode(const struct ctrl_cmd *cmd, uint8_t data[FRAME_DLC])
{
	uint16_t s = (uint16_t)cmd->steer;

	data[1] = (uint8_t)(s & 0xFF);
	data[2] = (uint8_t)(s >> 8);
	data[3] = cmd->throttle;
	data[4] = cmd->brake;
	data[5] = cmd->buttons;
	data[6] = crc8_j1850(data, 6);
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

static inline int catalog_sender(uint32_t id)
{
	switch (id) {
	case ID_CTRL:
	case ID_BLINK:
		return NODE_N1;
	case ID_FORCE:
		return NODE_N2;
	case ID_FAULT(NODE_N1):
	case ID_FAULT(NODE_N2):
	case ID_FAULT(NODE_N3):
		return (int)(id - ID_FAULT(0));
	case ID_HB(NODE_N1):
	case ID_HB(NODE_N2):
	case ID_HB(NODE_N3):
		return (int)(id - ID_HB(0));
	default:
		return -1;
	}
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
