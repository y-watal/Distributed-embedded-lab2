/* CAN message definitions shared by the Pi and both STM32 applications. */
#ifndef SHARED_PROTOCOL_H
#define SHARED_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum node_idx {
	NODE_N1 = 0, /* cockpit Pi */
	NODE_N2 = 1, /* front steering STM32 */
	NODE_N3 = 2, /* rear drivetrain STM32 */
	NUM_NODES = 3,
};

/* Lab 3 frames use standard 11-bit CAN IDs and an 8-byte payload. */
#define ID_CTRL     0x010
#define ID_FAULT(n) (0x020 + (n))
#define ID_HB(n)    (0x100 + (n))
#define ID_BLINK    0x200
#define ID_FORCE    0x300

/*
 * Lab 5 IDs are grouped in blocks of four: low two bits identify the sender
 * (N1=0, N2=1, N3=2). The fourth ID in each block is unused. Lower CAN IDs
 * win arbitration, so the order is deliberate:
 *   CTRL, FAULT  (0x010-0x022)  brake within 100 ms
 *   REF          (0x040-0x042)  one frame at 50 Hz; latency is time-sync error
 *   HB           (0x100-0x102)  44 ms degraded-bus deadline
 *   Raft bulk    (0x140-0x156)  fragmented, retried every 100 ms or later
 *   BLINK, FORCE (0x200, 0x300)
 * Raft bulk sits above HB so a burst of fragments never delays a heartbeat.
 */
#define ID_REF(n)           (0x040 + (n)) /* leader time and presence, every 20 ms */
#define ID_RAFT_BULK_BASE   0x140
#define ID_REQ_VOTE(n)      (0x140 + (n)) /* candidate -> all */
#define ID_VOTE_REPLY(n)    (0x144 + (n)) /* voter -> candidate */
#define ID_APPEND(n)        (0x148 + (n)) /* leader -> follower(s), log or empty */
#define ID_APPEND_ACK(n)    (0x14C + (n)) /* follower -> leader */
#define ID_CLIENT_UPDATE(n) (0x150 + (n)) /* local node/link event -> leader */
#define ID_CLIENT_REPLY(n)  (0x154 + (n)) /* commit ack or leader redirect */
#define ID_RAFT_BULK_LAST   ID_CLIENT_REPLY(NODE_N3)
/* Hardware/socket filters: REF block and the 32-ID bulk block. */
#define RAFT_REF_FILTER_MASK  0x7FC /* 0x040-0x043 */
#define RAFT_BULK_FILTER_MASK 0x7E0 /* 0x140-0x15F */
/* Compact slot per Raft ID: block number * 4 + sender. REF is block 0. */
#define RAFT_ID_SLOTS       (7 * 4)

#define FRAME_DLC 8
#define SEQ_MASK  0x0F

enum zone_state {
	ZONE_BOOT = 0,
	ZONE_SYS_ERROR = 1,
	ZONE_NORMAL = 2,
	ZONE_ISOLATED = 3,
};

/* HB_Nx: byte 1 carries state and local fault, byte 2 carries bus status. */
#define HB_B1_STATE_MASK  0x0F
#define HB_B1_LOCAL_FAULT (1u << 4)
#define HB_B2_A_DEGRADED (1u << 0)
#define HB_B2_B_DEGRADED (1u << 1)
#define HB_B2_A_BUSOFF   (1u << 2)
#define HB_B2_B_BUSOFF   (1u << 3)
#define HB_B2_RELAY      (1u << 4)

/* FAULT_Nx: byte 1 is the fault code and byte 2 is its detail. */
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

/* CTRL: steer bytes 1-2 LE, throttle 3, brake 4, buttons 5, CRC 6. */
struct ctrl_cmd {
	int16_t steer;
	uint8_t throttle;
	uint8_t brake;
	uint8_t buttons;
};
#define CTRL_STEER_MIN     (-32767)
#define CTRL_STEER_MAX     32766
#define CTRL_STEER_INVALID (-32768)
#define CTRL_PEDAL_SHIFT   7
#define CTRL_BTN4          (1u << 0)
#define CTRL_BTN5          (1u << 1)
#define CTRL_BTN10         (1u << 2)
#define CTRL_BTN_MASK      (CTRL_BTN4 | CTRL_BTN5 | CTRL_BTN10)
#define CTRL_CRC_LEN       6

/* BLINK: byte 1 carries left, right, and lamp phase. */
#define BLINK_LEFT  (1u << 0)
#define BLINK_RIGHT (1u << 1)
#define BLINK_PHASE (1u << 2)

/* CRC-8/SAE-J1850 for CTRL, including its sequence byte. */
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

static inline bool catalog_is_raft(uint32_t id)
{
	if ((id & 3u) >= NUM_NODES) {
		return false;
	}
	return (id & ~3u) == ID_REF(0) ||
	       (id >= ID_RAFT_BULK_BASE && id <= ID_RAFT_BULK_LAST);
}

/* 0..RAFT_ID_SLOTS-1 for a Raft ID, -1 otherwise. */
static inline int raft_id_slot(uint32_t id)
{
	if (!catalog_is_raft(id)) {
		return -1;
	}
	if ((id & ~3u) == ID_REF(0)) {
		return (int)(id & 3u);
	}
	return (int)(id - ID_RAFT_BULK_BASE) + 4;
}

/* Sender is encoded in Raft IDs and fixed by ID for Lab 3 messages. */
static inline int catalog_sender(uint32_t id)
{
	if (catalog_is_raft(id)) {
		return (int)(id & 3u);
	}

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

#endif /* SHARED_PROTOCOL_H */
