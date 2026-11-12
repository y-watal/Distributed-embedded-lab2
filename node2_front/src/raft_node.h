#ifndef RAFT_NODE_H
#define RAFT_NODE_H

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/drivers/can.h>

#include "raft_agent.h"

int raft_node_init(void);
void raft_node_receive(const struct can_frame *frame);
bool raft_node_report_event(struct raft_event event);
void raft_node_get_network(struct raft_network *out);
void raft_node_print_status(void);

#endif /* RAFT_NODE_H */
