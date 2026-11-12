#ifndef PI_RAFT_NODE_H
#define PI_RAFT_NODE_H

#include <stdbool.h>
#include <stdint.h>

#include "raft_agent.h"

typedef bool (*pi_raft_emit_fn)(uint32_t id, const uint8_t frame[FRAME_DLC]);

int raft_node_init(pi_raft_emit_fn emit);
void raft_node_receive(uint32_t id, const uint8_t frame[FRAME_DLC]);
bool raft_node_report_event(struct raft_event event);
void raft_node_get_network(struct raft_network *out);
void raft_node_print_status(void);

#endif /* PI_RAFT_NODE_H */
