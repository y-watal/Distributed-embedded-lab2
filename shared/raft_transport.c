#include "raft_transport.h"

#include <string.h>

bool raft_transport_send(struct raft_transport *t, uint32_t id, const uint8_t *body,
			 uint8_t len, raft_emit_frame_fn emit, void *emit_ctx)
{
	if (!catalog_is_raft(id) || !emit || !body ||
	    ((id & ~3u) == ID_REF(0) && len != RAFT_REF_LEN)) {
		return false;
	}
	if ((id & ~3u) == ID_REF(0)) {
		return emit(emit_ctx, id, body);
	}
	uint8_t transfer_id = ++t->next_transfer[raft_id_slot(id)];
	for (uint8_t fragment = 0; fragment < raft_fragment_count(len); fragment++) {
		uint8_t frame[FRAME_DLC];

		if (!raft_fragment_encode(transfer_id, fragment, body, len, frame) ||
		    !emit(emit_ctx, id, frame)) {
			return false;
		}
	}
	return true;
}

bool raft_transport_receive(struct raft_transport *t, uint32_t id,
			    const uint8_t frame[FRAME_DLC], uint32_t now_ms,
			    uint8_t body[RAFT_MAX_BODY_LEN], uint8_t *len)
{
	if (!catalog_is_raft(id) || !body || !len) {
		return false;
	}
	if ((id & ~3u) == ID_REF(0)) {
		memcpy(body, frame, RAFT_REF_LEN);
		*len = RAFT_REF_LEN;
		return true;
	}
	if (!raft_fragment_valid(frame)) {
		return false;
	}
	struct raft_rx_transfer *r = &t->rx[raft_id_slot(id)];
	uint8_t transfer_id = frame[0];
	uint8_t fragment = frame[1];
	uint8_t body_len = frame[2];
	bool timed_out = !r->valid || (uint32_t)(now_ms - r->last_ms) > RAFT_REASSEMBLY_TIMEOUT_MS;

	if (!timed_out && transfer_id != r->transfer_id &&
	    (int8_t)(transfer_id - r->transfer_id) <= 0) {
		return false; /* late fragment of an older transfer */
	}
	if (timed_out || transfer_id != r->transfer_id || body_len != r->body_len) {
		r->valid = true;
		r->complete = false;
		r->transfer_id = transfer_id;
		r->body_len = body_len;
		r->fragments = 0;
	}
	r->last_ms = now_ms;
	if (r->complete) {
		return false; /* second bus copy of a completed message */
	}
	for (uint8_t i = 0; i < RAFT_FRAGMENT_DATA; i++) {
		uint16_t offset = (uint16_t)fragment * RAFT_FRAGMENT_DATA + i;
		if (offset < body_len) {
			r->body[offset] = frame[3 + i];
		}
	}
	r->fragments |= (uint64_t)1 << fragment;
	uint8_t count = raft_fragment_count(body_len);
	uint64_t complete_mask = ((uint64_t)1 << count) - 1;

	if ((r->fragments & complete_mask) != complete_mask) {
		return false;
	}
	r->complete = true;
	memcpy(body, r->body, body_len);
	*len = body_len;
	return true;
}
