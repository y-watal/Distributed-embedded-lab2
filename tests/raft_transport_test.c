#include <assert.h>
#include <string.h>

#include "raft_transport.h"

struct captured {
	uint32_t id[8];
	uint8_t frame[8][FRAME_DLC];
	unsigned count;
};

static bool capture(void *ctx, uint32_t id, const uint8_t frame[FRAME_DLC])
{
	struct captured *c = ctx;

	assert(c->count < 8);
	c->id[c->count] = id;
	memcpy(c->frame[c->count++], frame, FRAME_DLC);
	return true;
}

int main(void)
{
	struct raft_transport tx = {0}, rx = {0};
	struct captured c = {0};
	struct raft_client_update update = {
		.target = NODE_N1,
		.request_id = 12345,
		.event = {RAFT_EVENT_CANLINK_DOWN, NODE_N3, 0, 0},
	};
	uint8_t body[RAFT_CLIENT_UPDATE_LEN];
	uint8_t assembled[RAFT_MAX_BODY_LEN];
	uint8_t len = 0;

	raft_client_update_encode(&update, body);
	assert(raft_transport_send(&tx, ID_CLIENT_UPDATE(NODE_N3), body,
				   sizeof(body), capture, &c));
	assert(c.count == 2 && c.id[0] == ID_CLIENT_UPDATE(NODE_N3));
	assert(!raft_transport_receive(&rx, c.id[1], c.frame[1], 10, assembled, &len));
	assert(!raft_transport_receive(&rx, c.id[1], c.frame[1], 11, assembled, &len));
	assert(raft_transport_receive(&rx, c.id[0], c.frame[0], 12, assembled, &len));
	assert(len == sizeof(body) && memcmp(assembled, body, len) == 0);
	assert(!raft_transport_receive(&rx, c.id[0], c.frame[0], 13, assembled, &len));

	const struct raft_ref ref = {123, 456, 7};
	uint8_t ref_body[RAFT_REF_LEN];
	raft_ref_encode(&ref, ref_body);
	c.count = 0;
	assert(raft_transport_send(&tx, ID_REF(NODE_N1), ref_body,
				   sizeof(ref_body), capture, &c));
	assert(c.count == 1);
	assert(raft_transport_receive(&rx, c.id[0], c.frame[0], 20, assembled, &len));
	assert(len == RAFT_REF_LEN && memcmp(assembled, ref_body, len) == 0);
	return 0;
}
