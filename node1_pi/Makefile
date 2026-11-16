CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=gnu11
CFLAGS += -I../shared
LDLIBS = -pthread

cockpit: cockpit.c raft_node.c raft_node.h gtime.c gtime.h ../shared/timesync.c ../shared/timesync.h ../shared/blink_phase.h catalog_pi.h state.h testpoints.h ../shared/protocol.h ../shared/raft_wire.h ../shared/raft_config.h ../shared/raft_agent.c ../shared/raft_agent.h ../shared/raft_transport.c ../shared/raft_transport.h
	$(CC) $(CFLAGS) -o $@ cockpit.c raft_node.c gtime.c ../shared/timesync.c ../shared/raft_agent.c ../shared/raft_transport.c $(LDLIBS)

clean:
	rm -f cockpit

.PHONY: clean
