#ifndef SHARED_RAFT_TRANSPORT_H
#define SHARED_RAFT_TRANSPORT_H

#include "raft_wire.h"

#define RAFT_REASSEMBLY_TIMEOUT_MS 250u

struct raft_rx_transfer {
	uint8_t transfer_id;
	uint8_t body_len;
	uint64_t fragments;
	uint32_t last_ms;
	bool valid;
	bool complete;
	uint8_t body[RAFT_MAX_BODY_LEN];
};

struct raft_transport {
	struct raft_rx_transfer rx[RAFT_ID_SLOTS];
	uint8_t next_transfer[RAFT_ID_SLOTS];
};

typedef bool (*raft_emit_frame_fn)(void *ctx, uint32_t id, const uint8_t frame[FRAME_DLC]);

/* A whole logical message is sent as one REF or several numbered fragments. */
bool raft_transport_send(struct raft_transport *t, uint32_t id, const uint8_t *body,
			 uint8_t len, raft_emit_frame_fn emit, void *emit_ctx);

/* True only when a complete body is ready. Duplicated CAN copies return false. */
bool raft_transport_receive(struct raft_transport *t, uint32_t id,
			    const uint8_t frame[FRAME_DLC], uint32_t now_ms,
			    uint8_t body[RAFT_MAX_BODY_LEN], uint8_t *len);

#endif /* SHARED_RAFT_TRANSPORT_H */
