#include <assert.h>
#include <string.h>

#include "raft_wire.h"

static void test_ref(void)
{
	const struct raft_ref in = {0x12345678u, 0xABCDu, 0x2345u};
	const uint8_t expected[RAFT_REF_LEN] = {0x78, 0x56, 0x34, 0x12,
					     0xCD, 0xAB, 0x45, 0x23};
	uint8_t bytes[RAFT_REF_LEN];
	struct raft_ref out;

	raft_ref_encode(&in, bytes);
	assert(memcmp(bytes, expected, sizeof(bytes)) == 0);
	raft_ref_decode(bytes, &out);
	assert(out.master_seconds == in.master_seconds);
	assert(out.fraction_65536 == in.fraction_65536);
	assert(out.cycle == in.cycle);
}

static void test_vote(void)
{
	const struct raft_vote_request in = {0x10203040u, 70000u, 3u};
	uint8_t body[RAFT_VOTE_REQUEST_LEN];
	struct raft_vote_request out;

	raft_vote_request_encode(&in, body);
	assert(body[0] == 0x40 && body[1] == 0x30 && body[2] == 0x20 && body[3] == 0x10);
	raft_vote_request_decode(body, &out);
	assert(out.term == in.term && out.last_index == in.last_index &&
	       out.last_term == in.last_term);

	const struct raft_vote_reply reply = {4u, NODE_N2, 1u};
	uint8_t reply_body[RAFT_VOTE_REPLY_LEN];
	struct raft_vote_reply reply_out;
	raft_vote_reply_encode(&reply, reply_body);
	raft_vote_reply_decode(reply_body, &reply_out);
	assert(reply_out.term == reply.term && reply_out.candidate == reply.candidate &&
	       reply_out.granted == reply.granted);
}

static void test_append_and_fragments(void)
{
	const struct raft_append in = {
		.target = NODE_N3,
		.term = 4,
		.prev_index = 70000,
		.prev_term = 3,
		.leader_commit = 69999,
		.entry_count = 1,
		.entry_term = 4,
		.entry = {RAFT_EVENT_CANLINK_DOWN, NODE_N3, 0, 0},
	};
	uint8_t body[RAFT_APPEND_ENTRY_LEN];
	uint8_t assembled[RAFT_APPEND_ENTRY_LEN] = {0};
	struct raft_append out;
	uint8_t len = raft_append_encode(&in, body);
	assert(len == RAFT_APPEND_ENTRY_LEN);
	assert(raft_fragment_count(len) == 6);

	for (uint8_t frag = 0; frag < raft_fragment_count(len); frag++) {
		uint8_t frame[FRAME_DLC];
		assert(raft_fragment_encode(0xA5, frag, body, len, frame));
		assert(raft_fragment_valid(frame));
		assert(frame[0] == 0xA5 && frame[1] == frag && frame[2] == len);
		for (uint8_t j = 0; j < RAFT_FRAGMENT_DATA; j++) {
			uint16_t offset = (uint16_t)frag * RAFT_FRAGMENT_DATA + j;
			if (offset < len) {
				assembled[offset] = frame[3 + j];
			}
		}
		if (frag == 5) {
			frame[7] = 1;
			assert(!raft_fragment_valid(frame));
		}
	}
	assert(memcmp(body, assembled, len) == 0);
	assert(raft_append_decode(assembled, len, &out));
	assert(out.target == in.target && out.term == in.term &&
	       out.prev_index == in.prev_index && out.prev_term == in.prev_term &&
	       out.leader_commit == in.leader_commit && out.entry_count == 1 &&
	       out.entry_term == in.entry_term && out.entry.type == in.entry.type &&
	       out.entry.node == in.entry.node && out.entry.bus == in.entry.bus);
	assert(!raft_append_decode(body, RAFT_APPEND_EMPTY_LEN, &out));

	struct raft_append empty = in;
	empty.entry_count = 0;
	len = raft_append_encode(&empty, body);
	assert(len == RAFT_APPEND_EMPTY_LEN);
	assert(raft_fragment_count(len) == 4);
	assert(raft_append_decode(body, len, &out) && out.entry_count == 0);
}

static void test_ack_and_client(void)
{
	const struct raft_append_ack ack = {4u, 70001u, NODE_N2, 1u};
	uint8_t ack_body[RAFT_APPEND_ACK_LEN];
	struct raft_append_ack ack_out;
	raft_append_ack_encode(&ack, ack_body);
	raft_append_ack_decode(ack_body, &ack_out);
	assert(ack_out.term == ack.term && ack_out.match_index == ack.match_index &&
	       ack_out.target == ack.target && ack_out.success == ack.success);

	const struct raft_client_update update = {
		NODE_N1, 0x12345678u, {RAFT_EVENT_CANLINK_DOWN, NODE_N3, 1, 0}
	};
	uint8_t update_body[RAFT_CLIENT_UPDATE_LEN];
	struct raft_client_update update_out;
	raft_client_update_encode(&update, update_body);
	raft_client_update_decode(update_body, &update_out);
	assert(update_out.target == update.target && update_out.request_id == update.request_id &&
	       update_out.event.type == update.event.type &&
	       update_out.event.node == update.event.node &&
	       update_out.event.bus == update.event.bus);

	const struct raft_client_reply reply = {
		NODE_N3, 0x12345678u, RAFT_CLIENT_COMMITTED, NODE_N1, 70001u
	};
	uint8_t reply_body[RAFT_CLIENT_REPLY_LEN];
	struct raft_client_reply reply_out;
	raft_client_reply_encode(&reply, reply_body);
	raft_client_reply_decode(reply_body, &reply_out);
	assert(reply_out.target == reply.target && reply_out.request_id == reply.request_id &&
	       reply_out.status == reply.status && reply_out.leader == reply.leader &&
	       reply_out.committed_index == reply.committed_index);
}

int main(void)
{
	assert(ID_CTRL < ID_FAULT(0) && ID_FAULT(2) < ID_REF(0));
	assert(catalog_is_raft(ID_REF(NODE_N1)));
	assert(catalog_sender(ID_APPEND(NODE_N3)) == NODE_N3);
	assert(!catalog_is_raft(0x043));
	test_ref();
	test_vote();
	test_append_and_fragments();
	test_ack_and_client();
	return 0;
}
