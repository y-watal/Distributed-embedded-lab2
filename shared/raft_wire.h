/* Lab 5 Raft wire format, shared by the Pi and both STM32 applications. */
#ifndef SHARED_RAFT_WIRE_H
#define SHARED_RAFT_WIRE_H

#include "protocol.h"

/*
 * REF is one complete 8-byte classic CAN frame. Every other Lab 5 message
 * uses the fragment envelope below. These frames do NOT use Lab 3's byte-0
 * 4-bit sequence convention. The Raft receive path must parse them separately.
 */
#define RAFT_REF_LEN          FRAME_DLC
#define RAFT_FRAGMENT_DATA    5u
#define RAFT_MAX_BODY_LEN     255u
#define RAFT_TARGET_ALL       0xffu
#define RAFT_BUS_NONE         0xffu

/* One log entry represents one change to the replicated network state. */
enum raft_event_type {
	RAFT_EVENT_COCKPIT_OK = 1,
	RAFT_EVENT_COCKPIT_ERROR = 2,
	RAFT_EVENT_NODE_UP = 3,
	RAFT_EVENT_NODE_DOWN = 4,
	RAFT_EVENT_CANLINK_UP = 5,
	RAFT_EVENT_CANLINK_DOWN = 6,
	RAFT_EVENT_APPLICATION = 7,
};

struct raft_event {
	uint8_t type;
	uint8_t node;  /* NODE_N1..NODE_N3, or event source */
	uint8_t bus;   /* 0=A, 1=B, RAFT_BUS_NONE if not a link event */
	uint8_t value; /* application event code, otherwise zero */
};

/* REF: master clock seconds, 1/65536-second fraction, 20-ms cycle count. */
struct raft_ref {
	uint32_t master_seconds; /* the leader's global-time epoch */
	uint16_t fraction_65536; /* resolution about 15.3 us */
	uint16_t cycle;          /* wraps modulo 65536 */
};

struct raft_vote_request {
	uint32_t term;
	uint32_t last_index;
	uint32_t last_term;
};

struct raft_vote_reply {
	uint32_t term;
	uint8_t candidate; /* reply target */
	uint8_t granted;   /* 0 or 1 */
};

/* APPEND carries zero or one entry. Entry index is prev_index + 1. */
struct raft_append {
	uint8_t target; /* NODE_N1..NODE_N3, or RAFT_TARGET_ALL */
	uint32_t term;
	uint32_t prev_index;
	uint32_t prev_term;
	uint32_t leader_commit;
	uint8_t entry_count; /* 0 or 1 */
	uint32_t entry_term;
	struct raft_event entry;
};

struct raft_append_ack {
	uint32_t term;
	uint32_t match_index; /* or next index to try when success = 0 */
	uint8_t target;       /* leader ID */
	uint8_t success;      /* 0 or 1 */
};

struct raft_client_update {
	uint8_t target; /* believed leader, or RAFT_TARGET_ALL if unknown */
	uint32_t request_id; /* monotonic per client, for retries and dedup */
	struct raft_event event;
};

enum raft_client_status {
	RAFT_CLIENT_COMMITTED = 1,
	RAFT_CLIENT_REDIRECT = 2,
	RAFT_CLIENT_REJECTED = 3,
};

struct raft_client_reply {
	uint8_t target; /* client node ID */
	uint32_t request_id;
	uint8_t status;
	uint8_t leader; /* leader hint, or RAFT_TARGET_ALL if unknown */
	uint32_t committed_index; /* zero unless status is COMMITTED */
};

#define RAFT_VOTE_REQUEST_LEN 12u
#define RAFT_VOTE_REPLY_LEN    6u
#define RAFT_APPEND_EMPTY_LEN 18u
#define RAFT_APPEND_ENTRY_LEN 26u
#define RAFT_APPEND_ACK_LEN   10u
#define RAFT_CLIENT_UPDATE_LEN 9u
#define RAFT_CLIENT_REPLY_LEN 11u

static inline void raft_put_u16(uint8_t *p, uint16_t x)
{
	p[0] = (uint8_t)x;
	p[1] = (uint8_t)(x >> 8);
}

static inline uint16_t raft_get_u16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline void raft_put_u32(uint8_t *p, uint32_t x)
{
	p[0] = (uint8_t)x;
	p[1] = (uint8_t)(x >> 8);
	p[2] = (uint8_t)(x >> 16);
	p[3] = (uint8_t)(x >> 24);
}

static inline uint32_t raft_get_u32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void raft_event_encode(const struct raft_event *e, uint8_t out[4])
{
	out[0] = e->type;
	out[1] = e->node;
	out[2] = e->bus;
	out[3] = e->value;
}

static inline void raft_event_decode(const uint8_t in[4], struct raft_event *e)
{
	e->type = in[0];
	e->node = in[1];
	e->bus = in[2];
	e->value = in[3];
}

static inline void raft_ref_encode(const struct raft_ref *m, uint8_t out[RAFT_REF_LEN])
{
	raft_put_u32(out, m->master_seconds);
	raft_put_u16(out + 4, m->fraction_65536);
	raft_put_u16(out + 6, m->cycle);
}

static inline void raft_ref_decode(const uint8_t in[RAFT_REF_LEN], struct raft_ref *m)
{
	m->master_seconds = raft_get_u32(in);
	m->fraction_65536 = raft_get_u16(in + 4);
	m->cycle = raft_get_u16(in + 6);
}

static inline void raft_vote_request_encode(const struct raft_vote_request *m,
					    uint8_t out[RAFT_VOTE_REQUEST_LEN])
{
	raft_put_u32(out, m->term);
	raft_put_u32(out + 4, m->last_index);
	raft_put_u32(out + 8, m->last_term);
}

static inline void raft_vote_request_decode(const uint8_t in[RAFT_VOTE_REQUEST_LEN],
					    struct raft_vote_request *m)
{
	m->term = raft_get_u32(in);
	m->last_index = raft_get_u32(in + 4);
	m->last_term = raft_get_u32(in + 8);
}

static inline void raft_vote_reply_encode(const struct raft_vote_reply *m,
					  uint8_t out[RAFT_VOTE_REPLY_LEN])
{
	raft_put_u32(out, m->term);
	out[4] = m->candidate;
	out[5] = m->granted;
}

static inline void raft_vote_reply_decode(const uint8_t in[RAFT_VOTE_REPLY_LEN],
					  struct raft_vote_reply *m)
{
	m->term = raft_get_u32(in);
	m->candidate = in[4];
	m->granted = in[5];
}

/* Returns the body length, or zero for an invalid entry count. */
static inline uint8_t raft_append_encode(const struct raft_append *m,
					 uint8_t out[RAFT_APPEND_ENTRY_LEN])
{
	if (m->entry_count > 1) {
		return 0;
	}
	out[0] = m->target;
	raft_put_u32(out + 1, m->term);
	raft_put_u32(out + 5, m->prev_index);
	raft_put_u32(out + 9, m->prev_term);
	raft_put_u32(out + 13, m->leader_commit);
	out[17] = m->entry_count;
	if (m->entry_count == 1) {
		raft_put_u32(out + 18, m->entry_term);
		raft_event_encode(&m->entry, out + 22);
		return RAFT_APPEND_ENTRY_LEN;
	}
	return RAFT_APPEND_EMPTY_LEN;
}

static inline bool raft_append_decode(const uint8_t *in, uint8_t len, struct raft_append *m)
{
	if ((len != RAFT_APPEND_EMPTY_LEN && len != RAFT_APPEND_ENTRY_LEN) ||
	    (in[17] == 0 && len != RAFT_APPEND_EMPTY_LEN) ||
	    (in[17] == 1 && len != RAFT_APPEND_ENTRY_LEN) || in[17] > 1) {
		return false;
	}
	m->target = in[0];
	m->term = raft_get_u32(in + 1);
	m->prev_index = raft_get_u32(in + 5);
	m->prev_term = raft_get_u32(in + 9);
	m->leader_commit = raft_get_u32(in + 13);
	m->entry_count = in[17];
	m->entry_term = 0;
	m->entry = (struct raft_event){0};
	if (m->entry_count == 1) {
		m->entry_term = raft_get_u32(in + 18);
		raft_event_decode(in + 22, &m->entry);
	}
	return true;
}

static inline void raft_append_ack_encode(const struct raft_append_ack *m,
					  uint8_t out[RAFT_APPEND_ACK_LEN])
{
	raft_put_u32(out, m->term);
	raft_put_u32(out + 4, m->match_index);
	out[8] = m->target;
	out[9] = m->success;
}

static inline void raft_append_ack_decode(const uint8_t in[RAFT_APPEND_ACK_LEN],
					  struct raft_append_ack *m)
{
	m->term = raft_get_u32(in);
	m->match_index = raft_get_u32(in + 4);
	m->target = in[8];
	m->success = in[9];
}

static inline void raft_client_update_encode(const struct raft_client_update *m,
					     uint8_t out[RAFT_CLIENT_UPDATE_LEN])
{
	out[0] = m->target;
	raft_put_u32(out + 1, m->request_id);
	raft_event_encode(&m->event, out + 5);
}

static inline void raft_client_update_decode(const uint8_t in[RAFT_CLIENT_UPDATE_LEN],
					     struct raft_client_update *m)
{
	m->target = in[0];
	m->request_id = raft_get_u32(in + 1);
	raft_event_decode(in + 5, &m->event);
}

static inline void raft_client_reply_encode(const struct raft_client_reply *m,
					    uint8_t out[RAFT_CLIENT_REPLY_LEN])
{
	out[0] = m->target;
	raft_put_u32(out + 1, m->request_id);
	out[5] = m->status;
	out[6] = m->leader;
	raft_put_u32(out + 7, m->committed_index);
}

static inline void raft_client_reply_decode(const uint8_t in[RAFT_CLIENT_REPLY_LEN],
					    struct raft_client_reply *m)
{
	m->target = in[0];
	m->request_id = raft_get_u32(in + 1);
	m->status = in[5];
	m->leader = in[6];
	m->committed_index = raft_get_u32(in + 7);
}

/*
 * Non-REF frames: byte 0 transfer ID, byte 1 fragment number (starting at 0),
 * byte 2 whole-body length, bytes 3-7 five successive body bytes, zero padded.
 * The sender and message type are in the CAN ID. A whole message is identified
 * by (sender, CAN ID, transfer ID). The same 8 bytes go on both CAN buses.
 */
static inline uint8_t raft_fragment_count(uint8_t body_len)
{
	return body_len == 0 ? 1 : (uint8_t)((body_len + RAFT_FRAGMENT_DATA - 1) /
					       RAFT_FRAGMENT_DATA);
}

static inline bool raft_fragment_encode(uint8_t transfer_id, uint8_t fragment,
					const uint8_t *body, uint8_t body_len,
					uint8_t out[FRAME_DLC])
{
	if (fragment >= raft_fragment_count(body_len) || (body_len != 0 && body == NULL)) {
		return false;
	}
	out[0] = transfer_id;
	out[1] = fragment;
	out[2] = body_len;
	for (uint8_t i = 0; i < RAFT_FRAGMENT_DATA; i++) {
		uint16_t offset = (uint16_t)fragment * RAFT_FRAGMENT_DATA + i;
		out[3 + i] = offset < body_len ? body[offset] : 0;
	}
	return true;
}

/* Checks the fragment header and padding. Reassembly belongs to the Raft RX task. */
static inline bool raft_fragment_valid(const uint8_t in[FRAME_DLC])
{
	if (in[1] >= raft_fragment_count(in[2])) {
		return false;
	}
	for (uint8_t i = 0; i < RAFT_FRAGMENT_DATA; i++) {
		uint16_t offset = (uint16_t)in[1] * RAFT_FRAGMENT_DATA + i;
		if (offset >= in[2] && in[3 + i] != 0) {
			return false;
		}
	}
	return true;
}

#endif /* SHARED_RAFT_WIRE_H */
