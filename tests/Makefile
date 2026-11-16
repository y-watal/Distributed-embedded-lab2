# Host tests for the shared Lab 5 code:  make -C tests
CC ?= gcc
CFLAGS ?= -std=c11 -O1 -g -Wall -Wextra
CFLAGS += -I../shared
OUT ?= build

TESTS = raft_agent raft_wire raft_transport raft_route timesync blink_phase raft_random

all: $(addprefix $(OUT)/,$(TESTS))
	@for t in $(TESTS); do ./$(OUT)/$$t > /dev/null && echo "PASS $$t" || { echo "FAIL $$t"; exit 1; }; done

$(OUT):
	mkdir -p $@

$(OUT)/raft_agent: raft_agent_test.c ../shared/raft_agent.c ../shared/*.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ raft_agent_test.c ../shared/raft_agent.c
$(OUT)/raft_transport: raft_transport_test.c ../shared/raft_transport.c ../shared/*.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ raft_transport_test.c ../shared/raft_transport.c
$(OUT)/timesync: timesync_test.c ../shared/timesync.c ../shared/*.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ timesync_test.c ../shared/timesync.c
# Random-leader mode with the seed that makes the cockpit win.
$(OUT)/raft_random: raft_random_test.c ../shared/raft_agent.c ../shared/*.h | $(OUT)
	$(CC) $(CFLAGS) -DRAFT_DEFAULT_LEADER=-1 -DRAFT_ELECTION_SEED=RAFT_SEED_PI_WINS \
		-o $@ raft_random_test.c ../shared/raft_agent.c
$(OUT)/%: %_test.c ../shared/*.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -rf $(OUT)

.PHONY: all clean
