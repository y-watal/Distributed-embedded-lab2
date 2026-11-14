#ifndef SHARED_RAFT_STATUS_H
#define SHARED_RAFT_STATUS_H

#include "raft_agent.h"

/*
 * What a console prints about the log: the running hash of the committed log
 * (comparable across nodes even after compaction), the snapshot boundary, and
 * the newest RAFT_PRINT_ENTRIES entries still in the ring.
 */
#define RAFT_PRINT_ENTRIES 16

struct raft_log_view {
	uint32_t snapshot_index; /* entries 1..snapshot_index are compacted */
	uint32_t first, last;    /* tail[] holds entries first..last */
	uint32_t commit;
	uint32_t hash;
	struct raft_log_entry tail[RAFT_PRINT_ENTRIES];
};

/* Call with the agent locked; printing happens after unlocking. */
static inline void raft_log_view_capture(const struct raft_agent *a, struct raft_log_view *v)
{
	uint32_t first = a->snapshot_index + 1;

	if (a->last_index >= RAFT_PRINT_ENTRIES &&
	    a->last_index - RAFT_PRINT_ENTRIES + 1 > first) {
		first = a->last_index - RAFT_PRINT_ENTRIES + 1;
	}
	v->snapshot_index = a->snapshot_index;
	v->first = first;
	v->last = a->last_index;
	v->commit = a->commit_index;
	v->hash = raft_log_hash(a);
	for (uint32_t i = first; i <= a->last_index; i++) {
		(void)raft_log_entry(a, i, &v->tail[i - first]);
	}
}

static inline const char *raft_state_text(bool known, bool up)
{
	return !known ? "?" : (up ? "UP" : "DOWN");
}

#endif
