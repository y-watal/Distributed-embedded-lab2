/*
 * Lab 3 message catalog. All frames are standard 11-bit IDs, DLC 8, sent on
 * both buses. Byte 0 of every payload: low nibble = 4-bit sequence counter
 * (one counter per ID, same value on both bus copies), high nibble = flags
 * (none defined, must be 0). CTRL follows the handout template (with Lab 2's
 * wheel values and a CRC-8); the other layouts follow the Lab 1 design
 * document. Full table: docs/lab3_design.md.
 */
#ifndef CATALOG_H
#define CATALOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

enum node_idx {
	NODE_N1 = 0, /* Pi, cockpit */
	NODE_N2 = 1, /* front STM32, steering */
	NODE_N3 = 2, /* rear STM32, drivetrain */
	NUM_NODES = 3,
};

#define ID_CTRL     0x010
#define ID_FAULT(n) (0x020 + (n)) /* FAULT_N1..N3: one ID per sender */
#define ID_HB(n)    (0x100 + (n)) /* HB_N1..N3 */
#define ID_BLINK    0x200
#define ID_FORCE    0x300

#define FRAME_DLC 8
#define SEQ_MASK  0x0F

/* ---- HB_Nx (Lab 1 HB_x layout) ---- */
enum zone_state {
	ZONE_BOOT = 0,
	ZONE_SYS_ERROR = 1,
	ZONE_NORMAL = 2,
	ZONE_ISOLATED = 3,
};
#define HB_B1_STATE_MASK  0x0F    /* byte 1 bits 3:0 */
#define HB_B1_LOCAL_FAULT BIT(4)
#define HB_B2_A_DEGRADED  BIT(0)  /* byte 2 */
#define HB_B2_B_DEGRADED  BIT(1)
#define HB_B2_A_BUSOFF    BIT(2)  /* bus-off since the last heartbeat */
#define HB_B2_B_BUSOFF    BIT(3)
#define HB_B2_RELAY       BIT(4)  /* node is relaying */
/* byte 3: most recent local fault code, 0 = none */

/* ---- FAULT_Nx (Lab 1 ERR_NOTIFY_x layout) ---- */
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
/* byte 1: fault code; byte 2: detail (zone index for PEER_LOST, else offending ID low byte) */

/*
 * ---- CTRL (handout template layout) ----
 *   byte 1-2  steer     int16 LE, Lab 2 raw value, -32767..32766, + = right
 *   byte 3    throttle  uint8, Lab 2 raw value >> 7 (0..32767 -> 0..255)
 *   byte 4    brake     uint8, Lab 2 raw value >> 7
 *   byte 5    buttons   CTRL_BTN_* bits, others must be 0
 *   byte 6    CRC-8/SAE-J1850 over bytes 0-5 (sequence byte included)
 *   byte 7    reserved, must be 0
 */
struct ctrl_cmd {
	int16_t steer;
	uint8_t throttle;
	uint8_t brake;
	uint8_t buttons;
};
#define CTRL_STEER_MIN    (-32767)
#define CTRL_STEER_MAX    32766
#define CTRL_PEDAL_SHIFT  7   /* Lab 2 raw pedal value >> 7 = payload counts */
#define CTRL_BTN4         BIT(0) /* wheel button 4 */
#define CTRL_BTN5         BIT(1) /* wheel button 5: left signal */
#define CTRL_BTN10        BIT(2) /* wheel button 10 */
#define CTRL_BTN_MASK     (CTRL_BTN4 | CTRL_BTN5 | CTRL_BTN10)
#define CTRL_CRC_LEN      6   /* bytes 0..5 */

/*
 * CRC-8/SAE-J1850: poly 0x1D, init 0xFF, final XOR 0xFF, no reflection
 * (AUTOSAR E2E profile 1). Check value: "123456789" -> 0x4B.
 */
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

/* ---- BLINK (Lab 1 BLINK_SYNC layout), byte 1 ---- */
#define BLINK_LEFT  BIT(0)
#define BLINK_RIGHT BIT(1) /* both set = hazard */
#define BLINK_PHASE BIT(2) /* 1 = active lamps on */

/* ---- FORCE: byte 1 = force, int8 (a dummy counter in this lab) ---- */

/* Sending node of a catalog ID, or -1 for an ID not in the catalog. */
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

static inline bool catalog_is_hb(uint32_t id)
{
	return id >= ID_HB(0) && id < ID_HB(NUM_NODES);
}

#endif /* CATALOG_H */
