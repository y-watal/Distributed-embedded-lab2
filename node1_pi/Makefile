CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=gnu11
CFLAGS += -I../shared
LDLIBS = -pthread

# Lab 5 startup mode, same on all three nodes:
#   make                      default leader = cockpit
#   make LEADER=random        election at boot (RAFT_DEFAULT_LEADER -1)
#   make LEADER=random SEED=408   reproducible timeouts; 408 makes the Pi win
ifeq ($(LEADER),random)
CFLAGS += -DRAFT_DEFAULT_LEADER=-1
endif
ifneq ($(SEED),)
CFLAGS += -DRAFT_ELECTION_SEED=$(SEED)u
endif
# Rebuild when those flags change.
FLAGS_STAMP := .build_flags
$(shell echo '$(CFLAGS)' | cmp -s - $(FLAGS_STAMP) || echo '$(CFLAGS)' > $(FLAGS_STAMP))

cockpit: $(FLAGS_STAMP) ../shared/raft_status.h ../shared/raft_route.h cockpit.c raft_node.c raft_node.h gtime.c gtime.h ../shared/timesync.c ../shared/timesync.h ../shared/blink_phase.h catalog_pi.h state.h testpoints.h ../shared/protocol.h ../shared/raft_wire.h ../shared/raft_config.h ../shared/raft_agent.c ../shared/raft_agent.h ../shared/raft_transport.c ../shared/raft_transport.h
	$(CC) $(CFLAGS) -o $@ cockpit.c raft_node.c gtime.c ../shared/timesync.c ../shared/raft_agent.c ../shared/raft_transport.c $(LDLIBS)

clean:
	rm -f cockpit $(FLAGS_STAMP)

.PHONY: clean
