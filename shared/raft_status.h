#ifndef SHARED_RAFT_STATUS_H
#define SHARED_RAFT_STATUS_H

#include "raft_agent.h"

/* Zephyr builds define RAFT_SNPRINTF as snprintk (cbprintf, small stack). */
#ifndef RAFT_SNPRINTF
#include <stdio.h>
#define RAFT_SNPRINTF snprintf
#endif

/* Committed entries printed with the log summary (the most recent ones). */
#define RAFT_PRINT_TAIL 16

/*
 * One committed entry as the checkoff names it: MEMBER (node and link
 * changes), EVENT (application events), COCKPIT (enter or leave failure mode).
 */
static inline int raft_entry_text(char *buf, size_t n, uint32_t index,
				  const struct raft_log_entry *e)
{
	const struct raft_event *v = &e->event;

	if (v->type == RAFT_EVENT_APPLICATION) {
		return RAFT_SNPRINTF(buf, n, "EVENT #%u term %u N%u %s", (unsigned)index,
				(unsigned)e->term, (unsigned)v->node + 1u,
				v->value == RAFT_APP_TEST_FAULT_ON ? "test fault on" :
				v->value == RAFT_APP_TEST_FAULT_OFF ? "test fault off" : "event");
	}
	if (v->type == RAFT_EVENT_COCKPIT_OK || v->type == RAFT_EVENT_COCKPIT_ERROR) {
		return RAFT_SNPRINTF(buf, n, "COCKPIT #%u term %u %s", (unsigned)index,
				(unsigned)e->term, raft_event_name(v->type));
	}
	if (v->bus < 2) {
		return RAFT_SNPRINTF(buf, n, "MEMBER #%u term %u %s N%u bus %c", (unsigned)index,
				(unsigned)e->term, raft_event_name(v->type),
				(unsigned)v->node + 1u, 'A' + v->bus);
	}
	return RAFT_SNPRINTF(buf, n, "MEMBER #%u term %u %s N%u", (unsigned)index,
			(unsigned)e->term, raft_event_name(v->type), (unsigned)v->node + 1u);
}

static inline const char *raft_state_text(bool known, bool up)
{
	return !known ? "?" : (up ? "UP" : "DOWN");
}

#endif
