#define _POSIX_C_SOURCE 200809L
#include "raft_node.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "raft_transport.h"
#include "raft_route.h"
#include "raft_status.h"
#include "gtime.h"
#include "testpoints.h"

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
/* bits 0-7: leader + 1 (0 = unknown); bit 8: this node leads */
static atomic_uint leader_snapshot;
static pi_raft_emit_fn emit_can;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static pthread_t worker;
static struct raft_work queue[RAFT_QUEUE_DEPTH];
static unsigned head, count;
static uint32_t start_ms;

/*
 * The cockpit's log display. Every committed entry is printed once as it is
 * applied; entries that arrived already committed (catch-up after an outage)
 * are marked "(replayed)". raft_seen.log (in the working directory) keeps
 * what was shown, so after the Pi restarts and rejoins, entries it displayed
 * before going down are not shown again: the held events appear exactly once,
 * then live events. A fresh cluster (this node takes term 1 at boot) clears it.
 */
#define SEEN_FILE "raft_seen.log"
struct seen_record {
	uint32_t term;
	uint8_t event[4];
};
static struct {
	struct seen_record *rec; /* loaded at start */
	uint32_t n;              /* records valid in rec[] and in the file */
	uint32_t skipped;        /* matched entries not shown again */
	bool diverged;           /* history differs from the file: show everything */
	bool boot_handled;
	FILE *f;
} seen;

static uint32_t monotonic_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)((uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000);
}

static double since_start_s(void)
{
	return (double)(monotonic_ms() - start_ms) / 1000.0;
}

/* RAFT_DEBUG: election and append messages. */
static void debug_line(void *ctx, const char *fmt, ...)
{
	char text[160];
	va_list ap;

	(void)ctx;
	va_start(ap, fmt);
	vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);
	printf("[N1 %.3fs] >> RAFT %s\n", since_start_s(), text);
}

static void seen_load(void)
{
	FILE *in = fopen(SEEN_FILE, "rb");
	struct seen_record r;

	seen.rec = calloc(RAFT_LOG_CAPACITY, sizeof(*seen.rec));
	while (in && seen.rec && seen.n < RAFT_LOG_CAPACITY && fread(&r, sizeof(r), 1, in) == 1) {
		seen.rec[seen.n++] = r;
	}
	if (in) {
		fclose(in);
	}
	seen.f = fopen(SEEN_FILE, "ab");
}

/* Keep only the first n records of the file. */
static void seen_truncate(uint32_t n)
{
	if (seen.f) {
		fflush(seen.f);
		if (ftruncate(fileno(seen.f), (off_t)n * (off_t)sizeof(struct seen_record)) != 0) {
			perror(SEEN_FILE);
		}
	}
	seen.n = n;
}

static void seen_append(const struct raft_log_entry *e)
{
	struct seen_record r = {e->term, {e->event.type, e->event.node, e->event.bus,
					   e->event.value}};

	if (seen.f && fwrite(&r, sizeof(r), 1, seen.f) == 1) {
		fflush(seen.f);
	}
	if (seen.rec && seen.n < RAFT_LOG_CAPACITY) {
		seen.rec[seen.n] = r;
	}
	seen.n++;
}

static void on_apply(void *ctx, uint32_t index, const struct raft_log_entry *e, bool replayed,
		     const struct raft_network *net)
{
	char text[96];

	(void)ctx;
	(void)net;
	if (!e) {
		/* Entries before this were compacted: the file can't be matched. */
		printf("[N1 %.3fs] >> SNAPSHOT through #%u (replayed)\n", since_start_s(), index);
		seen_truncate(0);
		seen.diverged = true;
		return;
	}
	struct seen_record r = {e->term, {e->event.type, e->event.node, e->event.bus,
					   e->event.value}};

	if (!seen.diverged && index <= seen.n && seen.rec && index <= RAFT_LOG_CAPACITY) {
		if (memcmp(&seen.rec[index - 1], &r, sizeof(r)) == 0) {
			seen.skipped++; /* shown before this restart */
			return;
		}
		seen.diverged = true; /* a different history from here on */
		seen_truncate(index - 1);
	}
	if (seen.skipped) {
		printf("[N1 %.3fs] >> (#1-#%u already shown before this restart)\n",
		       since_start_s(), seen.skipped);
		seen.skipped = 0;
	}
	raft_entry_text(text, sizeof(text), index, e);
	printf("[N1 %.3fs] >> %s%s\n", since_start_s(), text, replayed ? " (replayed)" : "");
	if (index > seen.n) {
		seen_append(e);
	}
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
		if (agent.boot_led && !seen.boot_handled) {
			/* Fresh cluster: the history in raft_seen.log is not ours. */
			seen.boot_handled = true;
			seen.diverged = true;
			seen_truncate(0);
		}
		atomic_store_explicit(&route_snapshot, raft_route_snapshot(&agent.network),
			memory_order_relaxed);
		atomic_store_explicit(&leader_snapshot, (unsigned)((agent.leader + 1) |
			(agent.role == RAFT_LEADER ? 0x100 : 0)), memory_order_relaxed);
		bool leads = agent.role == RAFT_LEADER;
		pthread_mutex_unlock(&lock);
		pi_tp_set(TP_LEADER, leads);
		pi_tp_set(TP_SYNC, gtime_is_synced());
	}
	return NULL;
}

int raft_node_init(pi_raft_emit_fn emit)
{
	struct timespec seed_time;

	clock_gettime(CLOCK_MONOTONIC, &seed_time);
	emit_can = emit;
	start_ms = monotonic_ms();
	seen_load();
	raft_agent_init(&agent, NODE_N1, send_body, NULL, monotonic_ms(),
			(uint32_t)seed_time.tv_nsec ^ (uint32_t)seed_time.tv_sec);
	agent.debug = debug_line;
	agent.on_apply = on_apply;
	if (agent.boot_listen) {
		printf("Raft N1: listening %u ms for a running cluster before taking term 1"
		       " (%u entries in %s)\n", (unsigned)RAFT_BOOT_LISTEN_MS, seen.n, SEEN_FILE);
	} else if (agent.leader < 0) {
		printf("Raft N1: term %u, leader unknown, role FOLLOWER\n", agent.term);
	} else {
		printf("Raft N1: term %u, leader N%d, role %s\n", agent.term,
		       agent.leader + 1,
		       agent.role == RAFT_LEADER ? "LEADER" : "FOLLOWER");
	}
	atomic_store(&leader_snapshot, (unsigned)((agent.leader + 1) |
		(agent.role == RAFT_LEADER ? 0x100 : 0)));
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

void raft_node_leader_snapshot(int *leader, bool *is_leader)
{
	unsigned v = atomic_load_explicit(&leader_snapshot, memory_order_relaxed);

	*leader = (int)(v & 0xFF) - 1;
	*is_leader = (v & 0x100) != 0;
}

void raft_node_print_status(void)
{
	uint32_t term, last_index, commit_index, log_base, log_hash;
	static struct raft_log_entry tail[RAFT_PRINT_TAIL];
	static uint32_t printed_last, printed_commit;
	static bool printed;
	uint32_t tail_from = 1, tail_n = 0;
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
	log_base = agent.log_base;
	log_hash = agent.applied_hash; /* FNV over every committed entry, compacted ones too */
	network = agent.network;
	print_log = !printed || printed_last != last_index || printed_commit != commit_index;
	if (print_log) {
		/* Only the most recent entries: the log can hold thousands. */
		tail_from = last_index > RAFT_PRINT_TAIL ? last_index - RAFT_PRINT_TAIL + 1 : 1;
		if (tail_from <= log_base) {
			tail_from = log_base + 1;
		}
		for (uint32_t i = tail_from; i <= last_index && tail_n < RAFT_PRINT_TAIL; i++) {
			if (raft_agent_entry(&agent, i, &tail[tail_n])) {
				tail_n++;
			}
		}
		printed_last = last_index;
		printed_commit = commit_index;
		printed = true;
	}
	pthread_mutex_unlock(&lock);
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
		if (tail_from > 1) {
			printf("    (#1-#%u not shown%s)\n", tail_from - 1,
			       log_base ? ", older ones compacted into the snapshot" : "");
		}
		for (uint32_t k = 0; k < tail_n; k++) {
			uint32_t i = tail_from + k;
			const struct raft_event *e = &tail[k].event;
			char bus = e->bus < 2 ? (char)('A' + e->bus) : '-';

			printf("    [%u] term %u %s %s N%u bus %c value %u\n",
			       i, tail[k].term,
			       i <= commit_index ? "COMMITTED" : "PENDING",
			       raft_event_name(e->type), (unsigned)e->node + 1,
			       bus, (unsigned)e->value);
		}
	}
}
