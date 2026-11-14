#ifndef SHARED_RAFT_STATUS_H
#define SHARED_RAFT_STATUS_H

#include "raft_agent.h"

/* Compare committed entries across consoles without hashing struct padding
 * or including an uncommitted suffix that may legitimately differ. */
static inline uint32_t raft_committed_log_hash(const struct raft_log_entry *log,
						 uint32_t commit_index)
{
	uint32_t hash = 2166136261u;

	for (uint32_t i = 0; i < commit_index; i++) {
		uint32_t term = log[i].term;
		const uint8_t event[] = {log[i].event.type, log[i].event.node,
			log[i].event.bus, log[i].event.value};

		for (int byte = 0; byte < 4; byte++) {
			hash = (hash ^ (uint8_t)(term >> (8 * byte))) * 16777619u;
		}
		for (int byte = 0; byte < 4; byte++) {
			hash = (hash ^ event[byte]) * 16777619u;
		}
	}
	return hash;
}

static inline const char *raft_state_text(bool known, bool up)
{
	return !known ? "?" : (up ? "UP" : "DOWN");
}

#endif
