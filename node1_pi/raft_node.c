#define _POSIX_C_SOURCE 200809L
#include "raft_node.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "raft_transport.h"
#include "raft_route.h"
#include "raft_status.h"

#define RAFT_QUEUE_DEPTH 64

struct raft_work {
	uint8_t kind;
	uint32_t id;
	uint8_t frame[FRAME_DLC];
	struct raft_event event;
};

static struct raft_agent agent;
static struct raft_transport transport;
static atomic_uint route_snapshot;
static pi_raft_emit_fn emit_can;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static pthread_t worker;
static struct raft_work queue[RAFT_QUEUE_DEPTH];
static unsigned head, count;

static uint32_t monotonic_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)((uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000);
}

static bool emit_frame(void *ctx, uint32_t id, const uint8_t frame[FRAME_DLC])
{
	(void)ctx;
	return emit_can && emit_can(id, frame);
}

static bool send_body(void *ctx, uint32_t id, const uint8_t *body, uint8_t len)
{
	(void)ctx;
	return raft_transport_send(&transport, id, body, len, emit_frame, NULL);
}

static bool enqueue(struct raft_work work)
{
	bool ok = false;

	pthread_mutex_lock(&lock);
	if (count < RAFT_QUEUE_DEPTH) {
		queue[(head + count) % RAFT_QUEUE_DEPTH] = work;
		count++;
		ok = true;
		pthread_cond_signal(&wake);
	}
	pthread_mutex_unlock(&lock);
	return ok;
}

static void *raft_worker(void *arg)
{
	uint8_t body[RAFT_MAX_BODY_LEN];
	uint8_t len;

	(void)arg;
	for (;;) {
		struct timespec until;
		struct raft_work work;
		bool got = false;
		uint32_t wait_ms = raft_agent_next_wake_ms(&agent, monotonic_ms());

		clock_gettime(CLOCK_REALTIME, &until);
		until.tv_nsec += (long)wait_ms * 1000000L;
		if (until.tv_nsec >= 1000000000) {
			until.tv_sec++;
			until.tv_nsec -= 1000000000;
		}
		pthread_mutex_lock(&lock);
		if (!count) {
			pthread_cond_timedwait(&wake, &lock, &until);
		}
		if (count) {
			work = queue[head];
			head = (head + 1) % RAFT_QUEUE_DEPTH;
			count--;
			got = true;
		}
		pthread_mutex_unlock(&lock);

		uint32_t now = monotonic_ms();
		pthread_mutex_lock(&lock);
		if (got && work.kind == 0 &&
		    raft_transport_receive(&transport, work.id, work.frame, now, body, &len)) {
			raft_agent_receive(&agent, work.id, body, len, now);
		} else if (got && work.kind == 1) {
			raft_client_observe(&agent, work.event, now);
		}
		raft_agent_poll(&agent, now);
		atomic_store_explicit(&route_snapshot, raft_route_snapshot(&agent.network),
			memory_order_relaxed);
		pthread_mutex_unlock(&lock);
	}
	return NULL;
}

int raft_node_init(pi_raft_emit_fn emit)
{
	struct timespec seed_time;

	clock_gettime(CLOCK_MONOTONIC, &seed_time);
	emit_can = emit;
	raft_agent_init(&agent, NODE_N1, send_body, NULL, monotonic_ms(),
			(uint32_t)seed_time.tv_nsec ^ (uint32_t)seed_time.tv_sec);
	if (agent.leader < 0) {
		printf("Raft N1: term %u, leader unknown, role FOLLOWER\n", agent.term);
	} else {
		printf("Raft N1: term %u, leader N%d, role %s\n", agent.term,
		       agent.leader + 1,
		       agent.role == RAFT_LEADER ? "LEADER" : "FOLLOWER");
	}
	if (pthread_create(&worker, NULL, raft_worker, NULL) != 0) {
		return -1;
	}
	pthread_detach(worker);
	raft_node_report_event((struct raft_event){RAFT_EVENT_NODE_UP, NODE_N1,
					     RAFT_BUS_NONE, 0});
	return 0;
}

void raft_node_receive(uint32_t id, const uint8_t frame[FRAME_DLC])
{
	struct raft_work work = {.kind = 0, .id = id};

	memcpy(work.frame, frame, FRAME_DLC);
	(void)enqueue(work);
}

bool raft_node_report_event(struct raft_event event)
{
	struct raft_work work = {.kind = 1, .event = event};

	return enqueue(work);
}

void raft_node_get_network(struct raft_network *out)
{
	pthread_mutex_lock(&lock);
	raft_client_get_network(&agent, out);
	pthread_mutex_unlock(&lock);
}

uint32_t raft_node_route_snapshot(void)
{
	return atomic_load_explicit(&route_snapshot, memory_order_relaxed);
}

void raft_node_print_status(void)
{
	uint32_t term, last_index, commit_index;
	static struct raft_log_entry log_snapshot[RAFT_WORKING_LOG_ENTRIES];
	static uint32_t printed_last, printed_commit;
	static bool printed;
	bool print_log;
	int leader;
	enum raft_role role;
	struct raft_network network;

	pthread_mutex_lock(&lock);
	term = agent.term;
	leader = agent.leader;
	role = agent.role;
	last_index = agent.last_index;
	commit_index = agent.commit_index;
	network = agent.network;
	print_log = !printed || printed_last != last_index || printed_commit != commit_index;
	if (print_log) {
		memcpy(log_snapshot, agent.log, last_index * sizeof(log_snapshot[0]));
		printed_last = last_index;
		printed_commit = commit_index;
		printed = true;
	}
	pthread_mutex_unlock(&lock);
	uint32_t log_hash = raft_committed_log_hash(log_snapshot, commit_index);
	if (leader < 0) {
		printf("  RAFT: leader=? term=%u role=%s | committed=%u total=%u | log=%08x\n", term,
		       role == RAFT_CANDIDATE ? "CANDIDATE" : "FOLLOWER",
		       commit_index, last_index, log_hash);
	} else {
		printf("  RAFT: leader=N%d term=%u role=%s | committed=%u total=%u | log=%08x\n",
		       leader + 1, term,
		       role == RAFT_LEADER ? "LEADER" : "FOLLOWER",
		       commit_index, last_index, log_hash);
	}
	printf("  GRAPH:");
	for (int n = 0; n < NUM_NODES; n++) {
		printf(" N%d:%s(A:%s B:%s)", n + 1,
		       raft_state_text(network.node_known[n], network.node_up[n]),
		       raft_state_text(network.link_known[n][0], network.link_up[n][0]),
		       raft_state_text(network.link_known[n][1], network.link_up[n][1]));
	}
	printf(" cockpit:%s\n", !network.cockpit_known ? "?" :
	       (network.cockpit_ok ? "OK" : "ERROR"));
	if (print_log) {
		printf("  LOG SNAPSHOT: %u entries, committed through #%u\n",
		       last_index, commit_index);
		for (uint32_t i = 0; i < last_index; i++) {
			const struct raft_event *e = &log_snapshot[i].event;
			char bus = e->bus < 2 ? (char)('A' + e->bus) : '-';

			printf("    [%u] term %u %s %s N%u bus %c value %u\n",
			       i + 1, log_snapshot[i].term,
			       i < commit_index ? "COMMITTED" : "PENDING",
			       raft_event_name(e->type), (unsigned)e->node + 1,
			       bus, (unsigned)e->value);
		}
	}
}
