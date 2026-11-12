/*
 * Lab 3 cockpit zone (N1) on the Raspberry Pi.
 *
 *   UDP_RX    (main thread, highest)  wheel packet -> CTRL on can0 and can1
 *                                      immediately (R4.4); blinker and test
 *                                      button logic after the writes
 *   timer     every 20 ms HB_N1; every 5 ms HB_MON (DEG_A/B, peers, zone
 *             state, ERR, UDP timeout), BLINK (master, 500 ms phase), FAULT_N1
 *   can_rx    both buses: heartbeats, FAULT_N2/N3, FORCE -> UDP to the proxy
 *   report    console summary once a second
 *   stdin     typed commands (fault injection, test button)
 *
 * Test points (testpoints.h): UDP_RX toggles when a wheel packet arrives,
 * CAN_TX_A/B on every frame written to that bus, CMD_TX right after the CTRL
 * write to bus A, DEG_A/B and ERR as levels.
 *
 * Build: make     Run: sudo ./cockpit [--rt 80 --cpu 3]   (see --help)
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/can.h>
#include <linux/can/error.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <dirent.h>
#include <libgen.h>
#include <limits.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "catalog_pi.h"
#include "state.h"
#include "testpoints.h"

/* ---- Timing, same values as the Nucleos' lab3_config.h ---- */
#define MS                  1000000ULL
#define HB_PERIOD_NS        (20 * MS)
#define HB_MAX_INTERVAL_NS  (22 * MS)
#define DEG_TIMEOUT_NS      (2 * HB_MAX_INTERVAL_NS) /* 44 ms */
#define FAIL_TIMEOUT_NS     (3 * HB_MAX_INTERVAL_NS) /* 66 ms */
#define MONITOR_PERIOD_NS   (5 * MS)
#define DUP_WINDOW_NS       (10 * MS)
#define UDP_TIMEOUT_NS      (90 * MS)  /* Lab 2's command freshness limit */
#define BLINK_HALF_NS       (500 * MS) /* 1 Hz blink (R2.4); catalog: toggle every 500 ms */
#define MIN_EVENT_GAP_NS    (20 * MS)  /* catalog: BLINK and FAULT at least 20 ms apart */
#define FAULT_CLEAR_HOLD_NS (HB_MAX_INTERVAL_NS + 3 * MS)

/* ---- Lab 2 wheel logic ---- */
#define TURN_THRESHOLD    5000 /* blinker arms past this steering */
#define RETURN_THRESHOLD  4000 /* ... and cancels when back inside this */
#define BUTTON_DEBOUNCE_NS (30 * MS)
#define DOUBLE_PRESS_NS   (350 * MS)

#define PACKET_SIZE (4 + sizeof(DIJOYSTATE2_t)) /* counter + joystick state */

enum { BUS_A = 0, BUS_B = 1, NUM_BUSES = 2 };
enum blink_mode { BLINK_OFF = 0, BLINK_MODE_LEFT = 1, BLINK_MODE_RIGHT = 2, BLINK_HAZARD = 3 };
static const char *const blink_names[] = {"off", "LEFT", "RIGHT", "HAZARD"};
static const char *const zone_names[] = {"BOOT", "SYS_ERROR", "NORMAL", "ISOLATED"};
static const int tp_tx[NUM_BUSES] = {TP_CAN_TX_A, TP_CAN_TX_B};
static const int tp_deg[NUM_BUSES] = {TP_DEG_A, TP_DEG_B};

/* ---- Options ---- */
static struct {
	const char *bind_addr;
	int udp_port;
	const char *force_host; /* NULL: reply to whoever sends the wheel packets */
	int force_port;
	const char *ifname[NUM_BUSES];
	int bitrate;
	int rt_prio; /* 0: normal scheduling */
	int cpu;     /* -1: no pinning */
	bool quiet;
	bool flush; /* bounce an interface when its bus goes degraded (opt-in) */
	bool if_given[2]; /* --can-a / --can-b given: don't auto-detect that one */
} opt = {"0.0.0.0", 8765, NULL, 8001, {"can0", "can1"}, 500000, 0, -1, false, false, {false, false}};
static bool test_mode; /* COCKPIT_TEST_FD_* in use: no real interfaces */

/* ---- Sockets ---- */
static int can_fd[NUM_BUSES] = {-1, -1};
static int udp_fd = -1;

/* ---- State shared between threads (lock) ---- */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t last_hb_ns[NUM_BUSES][NUM_NODES];
static bool deg[NUM_BUSES] = {true, true};
static bool peer_alive[NUM_NODES];
static bool peer_fault[NUM_NODES];
static uint8_t peer_fault_code[NUM_NODES];
static uint64_t peer_fault_ns[NUM_NODES];
static uint8_t peer_state[NUM_NODES]; /* from their heartbeats */
static enum zone_state zone = ZONE_BOOT;
static char zone_event[96]; /* set by monitor() on a zone change, printed by the timer thread */
static uint64_t t0_ns;      /* program start, for event timestamps */
static bool booted;
static uint8_t local_fault = FAULT_NONE, last_fault = FAULT_NONE;
static bool test_fault;        /* cockpit test button engaged */
static uint64_t last_udp_ns;
static bool have_udp;
static struct ctrl_cmd last_cmd;
static int32_t last_raw[3];
static enum blink_mode selected = BLINK_OFF; /* driver's choice; hazard overrides */
static uint64_t mode_event_ns;  /* J: when the blinker mode last changed (selected or zone) */
static uint64_t test_fault_ns;  /* J: when the cockpit test fault was engaged */
static struct sockaddr_in proxy_addr;
static bool have_proxy;

/* Read without the lock by the CTRL path (one byte, written by the timer). */
static atomic_bool bus_deg[NUM_BUSES] = {true, true};

/* ---- Fault injection and commands ---- */
static atomic_bool inject_malformed, inject_range, stop;

/* ---- Statistics ---- */
static atomic_uint udp_count, ctrl_sent, dup_count, fault_rx, force_fwd;
static atomic_int force_last;
static atomic_uint tx_ok[NUM_BUSES], tx_err[NUM_BUSES], tx_skip[NUM_BUSES], err_frames[NUM_BUSES];
static atomic_uint rx_count[NUM_BUSES][NUM_SLOTS];
static atomic_bool busoff[NUM_BUSES];
static atomic_ullong lat_sum_ns, lat_max_ns, lat_all_max_ns;
static atomic_ullong udp_gap_max_ns, udp_gap_all_max_ns; /* longest wait between wheel packets */
static atomic_uint udp_gaps_over; /* gaps longer than UDP_TIMEOUT_NS */
static atomic_uint lat_n;

/*
 * Queuing jitter J per message and bus: the triggering event -> write() of
 * that frame returned (the frame is in the socket queue; the mcp251x driver
 * loads it into the controller after that). Triggers:
 *   CTRL          the wheel packet arriving (t0, just before UDP_RX toggles)
 *   HB_N1         its 20 ms deadline
 *   BLINK toggle  its 500 ms deadline
 *   BLINK change  the wheel packet / zone change that changed the blinker mode
 *                 (includes the 5 ms timer tick and the 20 ms BLINK spacing)
 *   FAULT_N1      the fault becoming true (test button packet, wheel timeout
 *                 expiring, peer silent for 66 ms); includes the 20 ms spacing
 * Lock-free: the CTRL path only adds a clock read and a few atomics.
 */
enum { J_CTRL, J_FAULT_N1, J_HB_N1, J_BLINK_TOGGLE, J_BLINK_CHANGE, J_NUM };
static const char *const j_names[J_NUM] = {
	"CTRL        ", "FAULT_N1    ", "HB_N1       ", "BLINK toggle", "BLINK change"};
static atomic_ullong j_max[J_NUM][NUM_BUSES], j_max_win[J_NUM][NUM_BUSES], j_sum[J_NUM][NUM_BUSES];
static atomic_uint j_n[J_NUM][NUM_BUSES];
#define J_REPORT_EVERY 5 /* seconds; "last" in the table is this window */

static void atomic_max(atomic_ullong *a, unsigned long long v)
{
	unsigned long long prev = atomic_load(a);

	while (v > prev && !atomic_compare_exchange_weak(a, &prev, v)) {
	}
}

static void j_note(int msg, int bus, uint64_t release_ns, uint64_t queued_ns)
{
	if (msg < 0 || release_ns == 0 || queued_ns < release_ns) {
		return;
	}
	uint64_t j = queued_ns - release_ns;

	atomic_max(&j_max[msg][bus], j);
	atomic_max(&j_max_win[msg][bus], j);
	atomic_fetch_add(&j_sum[msg][bus], j);
	atomic_fetch_add(&j_n[msg][bus], 1);
}

static uint8_t tx_seq[NUM_SLOTS];

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void sleep_until(uint64_t t)
{
	struct timespec ts = {.tv_sec = (time_t)(t / 1000000000ULL),
			      .tv_nsec = (long)(t % 1000000000ULL)};

	while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {
	}
}

static uint8_t next_seq(uint32_t id)
{
	int slot = catalog_slot(id);

	return (uint8_t)(__atomic_fetch_add(&tx_seq[slot], 1, __ATOMIC_RELAXED) & SEQ_MASK);
}

/*
 * Queues one frame on one bus; skips a degraded bus unless include_degraded.
 * jmsg >= 0: record J = (write() returned) - release_ns for that message.
 */
static bool send_on_j(int bus, uint32_t id, const uint8_t data[FRAME_DLC], bool include_degraded,
		      int jmsg, uint64_t release_ns)
{
	struct can_frame f = {.can_id = id, .can_dlc = FRAME_DLC};

	if (!include_degraded && atomic_load_explicit(&bus_deg[bus], memory_order_relaxed)) {
		atomic_fetch_add(&tx_skip[bus], 1);
		return false;
	}
	memcpy(f.data, data, FRAME_DLC);
	tp_toggle(tp_tx[bus]);
	if (write(can_fd[bus], &f, sizeof(f)) != (ssize_t)sizeof(f)) {
		atomic_fetch_add(&tx_err[bus], 1); /* queue full (no ACK) or interface down */
		return false;
	}
	if (jmsg >= 0) {
		j_note(jmsg, bus, release_ns, now_ns());
	}
	atomic_fetch_add(&tx_ok[bus], 1);
	return true;
}

/* Sets the sequence number and queues on both buses back to back. */
static void send_both_j(uint32_t id, uint8_t data[FRAME_DLC], bool include_degraded, int jmsg,
			uint64_t release_ns)
{
	data[0] = next_seq(id);
	send_on_j(BUS_A, id, data, include_degraded, jmsg, release_ns);
	send_on_j(BUS_B, id, data, include_degraded, jmsg, release_ns);
}

/* ===================================================================== */
/* CAN setup                                                             */
/* ===================================================================== */

/*
 * Linux names the two MCP2515 interfaces can0/can1 in whatever order they
 * probe, which can change between boots. Bus A is the module on SPI0 CE0
 * (spi0.0) and bus B the one on CE1 (spi0.1) - that's the wiring - so pick
 * the interfaces by their SPI device, not by name.
 */
static void resolve_interfaces(void)
{
	static char found[NUM_BUSES][IFNAMSIZ];
	static const char *const spi_dev[NUM_BUSES] = {"spi0.0", "spi0.1"};
	DIR *d = opendir("/sys/class/net");
	struct dirent *e;

	if (!d) {
		return;
	}
	while ((e = readdir(d)) != NULL) {
		char path[PATH_MAX], target[PATH_MAX];
		ssize_t n;

		if (strncmp(e->d_name, "can", 3) != 0) {
			continue;
		}
		snprintf(path, sizeof(path), "/sys/class/net/%s/device", e->d_name);
		n = readlink(path, target, sizeof(target) - 1);
		if (n <= 0) {
			continue;
		}
		target[n] = '\0';
		for (int bus = 0; bus < NUM_BUSES; bus++) {
			if (strcmp(basename(target), spi_dev[bus]) == 0 &&
			    strlen(e->d_name) < IFNAMSIZ) {
				memcpy(found[bus], e->d_name, strlen(e->d_name) + 1);
			}
		}
	}
	closedir(d);
	for (int bus = 0; bus < NUM_BUSES; bus++) {
		if (found[bus][0] && !opt.if_given[bus]) {
			opt.ifname[bus] = found[bus];
		}
	}
	if (found[BUS_A][0] || found[BUS_B][0]) {
		printf("bus A = %s (SPI0 CE0), bus B = %s (SPI0 CE1)\n", opt.ifname[BUS_A],
		       opt.ifname[BUS_B]);
	}
}

static int can_open(const char *ifname)
{
	int fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
	struct ifreq ifr = {0};
	struct sockaddr_can addr = {0};

	if (fd < 0) {
		return -1;
	}
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
	if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
		close(fd);
		return -1;
	}
	addr.can_family = AF_CAN;
	addr.can_ifindex = ifr.ifr_ifindex;
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}

	/* Only what the cockpit consumes: the other zones' FAULT and HB, FORCE. */
	struct can_filter filters[] = {
		{ID_FAULT(NODE_N2), CAN_SFF_MASK | CAN_EFF_FLAG | CAN_RTR_FLAG},
		{ID_FAULT(NODE_N3), CAN_SFF_MASK | CAN_EFF_FLAG | CAN_RTR_FLAG},
		{ID_HB(NODE_N2), CAN_SFF_MASK | CAN_EFF_FLAG | CAN_RTR_FLAG},
		{ID_HB(NODE_N3), CAN_SFF_MASK | CAN_EFF_FLAG | CAN_RTR_FLAG},
		{ID_FORCE, CAN_SFF_MASK | CAN_EFF_FLAG | CAN_RTR_FLAG},
	};
	setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FILTER, filters, sizeof(filters));
	can_err_mask_t err_mask = CAN_ERR_BUSOFF | CAN_ERR_CRTL | CAN_ERR_RESTARTED;
	setsockopt(fd, SOL_CAN_RAW, CAN_RAW_ERR_FILTER, &err_mask, sizeof(err_mask));

	/* Never block the brake path on a bus nobody ACKs. */
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	return fd;
}

/*
 * Optional (--flush, needs root): a link with no ACK keeps retrying what's
 * queued; when it comes back, those stale frames go out first. Bouncing the
 * interface drops them. Off by default: if `ip` fails to bring the interface
 * back up, the cockpit goes deaf on that bus. Runs in its own thread so the
 * timer isn't held up by `ip`.
 */
static void *flush_thread(void *arg)
{
	int bus = (int)(intptr_t)arg;
	char cmd[160];

	snprintf(cmd, sizeof(cmd),
		 "ip link set %s down && ip link set %s up type can bitrate %d restart-ms 100",
		 opt.ifname[bus], opt.ifname[bus], opt.bitrate);
	if (system(cmd) != 0) {
		fprintf(stderr, "flush of %s failed: run ./can_up.sh\n", opt.ifname[bus]);
	}
	return NULL;
}

/* "up", "DOWN" or "?" for the report. */
static const char *if_state(int bus)
{
	struct ifreq ifr = {0};

	if (test_mode) {
		return "up";
	}
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", opt.ifname[bus]);
	if (ioctl(can_fd[bus], SIOCGIFFLAGS, &ifr) < 0) {
		return "?";
	}
	return (ifr.ifr_flags & IFF_UP) && (ifr.ifr_flags & IFF_RUNNING) ? "up" : "DOWN";
}

static void flush_bus(int bus)
{
	pthread_t t;

	if (!opt.flush || test_mode || geteuid() != 0) {
		return;
	}
	if (pthread_create(&t, NULL, flush_thread, (void *)(intptr_t)bus) == 0) {
		pthread_detach(t);
	}
}

/* ===================================================================== */
/* UDP_RX: the brake path                                                */
/* ===================================================================== */

static void record_latency(uint64_t ns)
{
	unsigned long long prev;

	atomic_fetch_add(&lat_sum_ns, ns);
	atomic_fetch_add(&lat_n, 1);
	prev = atomic_load(&lat_max_ns);
	while (ns > prev && !atomic_compare_exchange_weak(&lat_max_ns, &prev, ns)) {
	}
	prev = atomic_load(&lat_all_max_ns);
	while (ns > prev && !atomic_compare_exchange_weak(&lat_all_max_ns, &prev, ns)) {
	}
}

/* Lab 2 blinkers.c and the self-test button, after the CTRL is out. lock held. */
static void wheel_logic(int32_t steer, bool b4, bool b5, bool b10, uint64_t now)
{
	static bool prev_b4, prev_b5, prev_b10, armed, exit_pending;
	static uint64_t last_b10_ns, first_exit_ns;
	bool right_press = b4 && !prev_b4;
	bool left_press = b5 && !prev_b5;
	bool test_press = b10 && !prev_b10 && now - last_b10_ns >= BUTTON_DEBOUNCE_NS;

	prev_b4 = b4;
	prev_b5 = b5;
	prev_b10 = b10;

	/* Button 10: single press = cockpit test fault, double press = recover. */
	if (test_press) {
		last_b10_ns = now;
		if (!test_fault) {
			test_fault = true;
			test_fault_ns = now;
			exit_pending = false;
		} else if (exit_pending && now - first_exit_ns <= DOUBLE_PRESS_NS) {
			test_fault = false;
			exit_pending = false;
		} else {
			first_exit_ns = now;
			exit_pending = true;
		}
	}
	if (exit_pending && now - first_exit_ns > DOUBLE_PRESS_NS) {
		exit_pending = false;
	}

	/* Turn signals: only while NORMAL (otherwise hazards). */
	if (zone != ZONE_NORMAL) {
		selected = BLINK_OFF;
		armed = false;
		return;
	}
	if (right_press) {
		selected = selected == BLINK_MODE_RIGHT ? BLINK_OFF : BLINK_MODE_RIGHT;
		armed = false;
	}
	if (left_press) {
		selected = selected == BLINK_MODE_LEFT ? BLINK_OFF : BLINK_MODE_LEFT;
		armed = false;
	}
	/* Cancel after the turn: arm past the threshold, cancel back near center. */
	if (selected == BLINK_MODE_RIGHT) {
		if (steer >= TURN_THRESHOLD) {
			armed = true;
		} else if (armed && steer <= RETURN_THRESHOLD) {
			selected = BLINK_OFF;
			armed = false;
		}
	} else if (selected == BLINK_MODE_LEFT) {
		if (steer <= -TURN_THRESHOLD) {
			armed = true;
		} else if (armed && steer >= -RETURN_THRESHOLD) {
			selected = BLINK_OFF;
			armed = false;
		}
	}
}

static void udp_loop(void)
{
	uint8_t buf[PACKET_SIZE + 16];
	DIJOYSTATE2_t js;

	while (!atomic_load(&stop)) {
		struct sockaddr_in from;
		socklen_t from_len = sizeof(from);
		ssize_t n = recvfrom(udp_fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);

		if (n < (ssize_t)PACKET_SIZE) {
			continue; /* short read, timeout (stop check), EINTR or error */
		}
		uint64_t t0 = now_ns();

		tp_toggle(TP_UDP_RX);
		memcpy(&js, buf + 4, sizeof(js));

		bool b4 = js.rgbButtons[4] != 0, b5 = js.rgbButtons[5] != 0;
		bool b10 = js.rgbButtons[10] != 0;
		struct ctrl_cmd cmd;
		uint8_t data[FRAME_DLC];

		ctrl_from_lab2(js.lX, js.lY, js.lRz, b4, b5, b10, &cmd);
		if (atomic_exchange(&inject_range, false)) {
			cmd.steer = CTRL_STEER_INVALID; /* receivers: FAULT_CTRL_RANGE */
		}
		data[0] = next_seq(ID_CTRL);
		ctrl_encode(&cmd, data);
		if (atomic_exchange(&inject_malformed, false)) {
			data[6] ^= 0x5A; /* bad CRC; receivers: FAULT_CTRL_MALFORMED */
		}

		send_on_j(BUS_A, ID_CTRL, data, false, J_CTRL, t0);
		tp_toggle(TP_CMD_TX);
		send_on_j(BUS_B, ID_CTRL, data, false, J_CTRL, t0);
		record_latency(now_ns() - t0);
		atomic_fetch_add(&ctrl_sent, 1);
		atomic_fetch_add(&udp_count, 1);

		/* Everything below is off the brake path. */
		pthread_mutex_lock(&lock);
		if (have_udp) {
			unsigned long long gap = t0 - last_udp_ns, prev;

			prev = atomic_load(&udp_gap_max_ns);
			while (gap > prev && !atomic_compare_exchange_weak(&udp_gap_max_ns, &prev, gap)) {
			}
			prev = atomic_load(&udp_gap_all_max_ns);
			while (gap > prev && !atomic_compare_exchange_weak(&udp_gap_all_max_ns, &prev, gap)) {
			}
			if (gap >= UDP_TIMEOUT_NS) {
				atomic_fetch_add(&udp_gaps_over, 1);
			}
		}
		last_udp_ns = t0;
		have_udp = true;
		last_cmd = cmd;
		last_raw[0] = js.lX;
		last_raw[1] = js.lY;
		last_raw[2] = js.lRz;
		if (!opt.force_host) {
			proxy_addr = from;
			proxy_addr.sin_port = htons((uint16_t)opt.force_port);
			have_proxy = true;
		}
		enum blink_mode sel_before = selected;

		wheel_logic(js.lX, b4, b5, b10, t0);
		if (selected != sel_before && zone == ZONE_NORMAL) {
			mode_event_ns = t0;
		}
		pthread_mutex_unlock(&lock);
	}
}

/* ===================================================================== */
/* CAN receive: heartbeats, FAULT, FORCE                                 */
/* ===================================================================== */

static void *can_rx_thread(void *arg)
{
	struct pollfd pfd[NUM_BUSES];
	struct {
		uint8_t seq;
		uint64_t ns;
		bool valid;
	} seen[NUM_SLOTS] = {0};

	(void)arg;
	for (int b = 0; b < NUM_BUSES; b++) {
		pfd[b].fd = can_fd[b];
		pfd[b].events = POLLIN;
	}

	while (!atomic_load(&stop)) {
		if (poll(pfd, NUM_BUSES, 200) <= 0) {
			continue;
		}
		for (int bus = 0; bus < NUM_BUSES; bus++) {
			struct can_frame f;

			if (!(pfd[bus].revents & POLLIN)) {
				continue;
			}
			while (read(can_fd[bus], &f, sizeof(f)) == (ssize_t)sizeof(f)) {
				uint64_t now = now_ns();

				if (f.can_id & CAN_ERR_FLAG) {
					atomic_fetch_add(&err_frames[bus], 1);
					if (f.can_id & CAN_ERR_BUSOFF) {
						atomic_store(&busoff[bus], true);
					}
					continue;
				}
				uint32_t id = f.can_id & CAN_SFF_MASK;
				int slot = catalog_slot(id);
				int sender = catalog_sender(id);

				if (slot < 0 || f.can_dlc != FRAME_DLC || sender <= NODE_N1) {
					continue;
				}
				atomic_fetch_add(&rx_count[bus][slot], 1);

				/* Presence counts every copy, relayed ones included. */
				if (id == (uint32_t)ID_HB(sender)) {
					pthread_mutex_lock(&lock);
					last_hb_ns[bus][sender] = now;
					pthread_mutex_unlock(&lock);
				}

				uint8_t seq = f.data[0] & SEQ_MASK;

				if (seen[slot].valid && seen[slot].seq == seq &&
				    now - seen[slot].ns < DUP_WINDOW_NS) {
					atomic_fetch_add(&dup_count, 1);
					continue;
				}
				seen[slot].seq = seq;
				seen[slot].ns = now;
				seen[slot].valid = true;

				if (id == (uint32_t)ID_HB(sender)) {
					bool lf = f.data[1] & HB_B1_LOCAL_FAULT;

					pthread_mutex_lock(&lock);
					peer_state[sender] = f.data[1] & HB_B1_STATE_MASK;
					if (lf) {
						peer_fault[sender] = true;
						peer_fault_code[sender] = f.data[3];
					} else if (now - peer_fault_ns[sender] > FAULT_CLEAR_HOLD_NS) {
						/* (an older HB can overtake a FAULT; hold the fault briefly) */
						peer_fault[sender] = false;
					}
					pthread_mutex_unlock(&lock);
				} else if (id == (uint32_t)ID_FAULT(sender)) {
					atomic_fetch_add(&fault_rx, 1);
					printf("FAULT from N%d: code 0x%02x detail 0x%02x\n", sender + 1,
					       f.data[1], f.data[2]);
					if (f.data[1] != FAULT_PEER_LOST) { /* PEER_LOST is informational */
						pthread_mutex_lock(&lock);
						peer_fault[sender] = true;
						peer_fault_code[sender] = f.data[1];
						peer_fault_ns[sender] = now;
						pthread_mutex_unlock(&lock);
					}
				} else if (id == ID_FORCE) {
					int8_t force = (int8_t)f.data[1];
					struct sockaddr_in to;
					bool ok;

					atomic_store(&force_last, force);
					pthread_mutex_lock(&lock);
					to = proxy_addr;
					ok = have_proxy;
					pthread_mutex_unlock(&lock);
					if (ok && sendto(udp_fd, &force, 1, 0, (struct sockaddr *)&to,
							 sizeof(to)) == 1) {
						atomic_fetch_add(&force_fwd, 1);
					}
				}
			}
		}
	}
	return NULL;
}

/* ===================================================================== */
/* Timer: HB_TX, HB_MON, BLINK, FAULT_N1                                 */
/* ===================================================================== */

static bool heard(int bus, int node, uint64_t now, uint64_t timeout)
{
	return last_hb_ns[bus][node] != 0 && now - last_hb_ns[bus][node] <= timeout;
}

static void send_hb(uint64_t release_ns)
{
	uint8_t d[FRAME_DLC] = {0};

	pthread_mutex_lock(&lock);
	d[1] = (uint8_t)((zone & HB_B1_STATE_MASK) | (local_fault ? HB_B1_LOCAL_FAULT : 0));
	d[2] = (uint8_t)((deg[BUS_A] ? HB_B2_A_DEGRADED : 0) | (deg[BUS_B] ? HB_B2_B_DEGRADED : 0));
	d[3] = local_fault ? local_fault : last_fault; /* most recent local fault */
	pthread_mutex_unlock(&lock);
	d[2] |= (uint8_t)((atomic_exchange(&busoff[BUS_A], false) ? HB_B2_A_BUSOFF : 0) |
			  (atomic_exchange(&busoff[BUS_B], false) ? HB_B2_B_BUSOFF : 0));
	/* heartbeats go out on degraded buses too */
	send_both_j(ID_HB(NODE_N1), d, true, J_HB_N1, release_ns);
}

static void send_fault(uint8_t code, uint8_t detail, uint64_t release_ns)
{
	uint8_t d[FRAME_DLC] = {0};

	d[1] = code;
	d[2] = detail;
	send_both_j(ID_FAULT(NODE_N1), d, false, J_FAULT_N1, release_ns);
	printf("FAULT_N1 sent: code 0x%02x detail 0x%02x\n", code, detail);
}

#define FAULT_QUEUE 8
static struct {
	uint8_t code, detail;
	uint64_t t; /* when the fault became true: J's start */
} fault_q[FAULT_QUEUE];
static int fault_q_len;

static void queue_fault(uint8_t code, uint8_t detail, uint64_t t)
{
	if (fault_q_len < FAULT_QUEUE) {
		fault_q[fault_q_len].code = code;
		fault_q[fault_q_len].detail = detail;
		fault_q[fault_q_len].t = t;
		fault_q_len++;
	}
}

/* HB_MON. lock held. */
static void monitor(uint64_t now)
{
	bool all_alive = true, any_peer_fault = false;

	for (int bus = 0; bus < NUM_BUSES; bus++) {
		bool heard_any = heard(bus, NODE_N2, now, DEG_TIMEOUT_NS) ||
				 heard(bus, NODE_N3, now, DEG_TIMEOUT_NS);
		bool d = !heard_any;

		if (d != deg[bus]) {
			deg[bus] = d;
			atomic_store(&bus_deg[bus], d);
			tp_set(tp_deg[bus], d);
			if (d) {
				flush_bus(bus);
			}
		}
	}
	for (int n = NODE_N2; n <= NODE_N3; n++) {
		bool alive = heard(BUS_A, n, now, FAIL_TIMEOUT_NS) ||
			     heard(BUS_B, n, now, FAIL_TIMEOUT_NS);

		if (booted && peer_alive[n] && !alive) {
			uint64_t last = last_hb_ns[BUS_A][n] > last_hb_ns[BUS_B][n] ? last_hb_ns[BUS_A][n]
									      : last_hb_ns[BUS_B][n];

			queue_fault(FAULT_PEER_LOST, (uint8_t)n, last + FAIL_TIMEOUT_NS); /* informational */
		}
		peer_alive[n] = alive;
		all_alive = all_alive && alive;
		/* a zone reporting a fault, or not ready (BOOT / ISOLATED), keeps us in error */
		any_peer_fault = any_peer_fault ||
				 (alive && (peer_fault[n] || peer_state[n] == ZONE_BOOT ||
					    peer_state[n] == ZONE_ISOLATED));
	}

	bool udp_ok = have_udp && now - last_udp_ns < UDP_TIMEOUT_NS;
	uint8_t lf = test_fault ? FAULT_TEST_BUTTON : (udp_ok ? FAULT_NONE : FAULT_UDP_TIMEOUT);

	if (lf != local_fault) {
		if (lf != FAULT_NONE && booted) {
			uint64_t t = now;

			if (lf == FAULT_TEST_BUTTON && test_fault_ns) {
				t = test_fault_ns;
			} else if (lf == FAULT_UDP_TIMEOUT && have_udp) {
				t = last_udp_ns + UDP_TIMEOUT_NS;
			}
			queue_fault(lf, 0, t);
		}
		if (lf != FAULT_NONE) {
			last_fault = lf;
		}
		local_fault = lf;
	}
	if (!booted && all_alive && udp_ok) {
		booted = true;
	}
	enum zone_state prev = zone;

	enum zone_state was = zone;

	zone = !booted ? ZONE_BOOT
		       : ((local_fault || !all_alive || any_peer_fault) ? ZONE_SYS_ERROR : ZONE_NORMAL);
	tp_set(TP_ERR, zone != ZONE_NORMAL);
	if ((was == ZONE_NORMAL) != (zone == ZONE_NORMAL)) {
		mode_event_ns = now; /* hazards on / off: a BLINK change */
	}

	if (zone != prev) {
		/* Record why; the caller prints it after dropping the lock. */
		int len = snprintf(zone_event, sizeof(zone_event), "zone %s -> %s:", zone_names[prev],
				   zone_names[zone]);

		if (local_fault && len < (int)sizeof(zone_event)) {
			len += snprintf(zone_event + len, sizeof(zone_event) - (size_t)len,
					" local 0x%02x%s", local_fault,
					local_fault == FAULT_UDP_TIMEOUT ? " (no wheel data)" : "");
		}
		for (int n = NODE_N2; n <= NODE_N3 && len < (int)sizeof(zone_event); n++) {
			size_t room = sizeof(zone_event) - (size_t)len;

			if (!peer_alive[n]) {
				len += snprintf(zone_event + len, room, " N%d lost (last HB A %lld ms, B %lld ms ago)",
						n + 1,
						last_hb_ns[BUS_A][n] ? (long long)((now - last_hb_ns[BUS_A][n]) / MS) : -1LL,
						last_hb_ns[BUS_B][n] ? (long long)((now - last_hb_ns[BUS_B][n]) / MS) : -1LL);
			} else if (peer_fault[n]) {
				len += snprintf(zone_event + len, room, " N%d fault 0x%02x", n + 1,
						peer_fault_code[n]);
			} else if (peer_state[n] == ZONE_BOOT || peer_state[n] == ZONE_ISOLATED) {
				len += snprintf(zone_event + len, room, " N%d %s", n + 1,
						zone_names[peer_state[n]]);
			}
		}
		if (len > 0 && len < (int)sizeof(zone_event) && zone_event[len - 1] == ':') {
			zone_event[len - 1] = '\0';
		}
	}
}

static void *timer_thread(void *arg)
{
	uint64_t now = now_ns();
	uint64_t next_hb = now, next_mon = now;
	uint64_t last_fault_ns = 0, last_blink_ns = 0, next_toggle = now;
	enum blink_mode sent_mode = BLINK_OFF;
	bool phase = false;

	(void)arg;
	while (!atomic_load(&stop)) {
		sleep_until(next_hb < next_mon ? next_hb : next_mon);
		now = now_ns();

		if (now >= next_hb) {
			send_hb(next_hb); /* J measured from the deadline */
			next_hb += HB_PERIOD_NS;
		}
		if (now < next_mon) {
			continue;
		}
		next_mon += MONITOR_PERIOD_NS;

		pthread_mutex_lock(&lock);
		monitor(now);
		enum blink_mode mode = zone == ZONE_NORMAL ? selected : BLINK_HAZARD;
		uint64_t mode_t = mode_event_ns;
		char event[sizeof(zone_event)];

		memcpy(event, zone_event, sizeof(event));
		zone_event[0] = '\0';
		pthread_mutex_unlock(&lock);

		if (event[0] && !opt.quiet) {
			printf("[N1 %.3fs] >> %s\n", (double)(now - t0_ns) / 1e9, event);
		}

		/* BLINK master: on change (phase on), then a toggle every 500 ms. */
		bool send = false;
		int blink_j = -1;
		uint64_t blink_rel = 0;

		if (mode != sent_mode && now - last_blink_ns >= MIN_EVENT_GAP_NS) {
			blink_j = J_BLINK_CHANGE;
			blink_rel = mode_t;
			sent_mode = mode;
			phase = mode != BLINK_OFF;
			next_toggle = now + BLINK_HALF_NS;
			send = true;
		} else if (now >= next_toggle) {
			blink_j = J_BLINK_TOGGLE;
			blink_rel = next_toggle;
			phase = sent_mode != BLINK_OFF && !phase;
			next_toggle += BLINK_HALF_NS;
			send = true; /* also refreshes "off" every 500 ms */
		}
		if (send) {
			uint8_t d[FRAME_DLC] = {0};
			static const uint8_t bits[] = {0, BLINK_LEFT, BLINK_RIGHT, BLINK_LEFT | BLINK_RIGHT};

			d[1] = (uint8_t)(bits[sent_mode] | (phase ? BLINK_PHASE : 0));
			send_both_j(ID_BLINK, d, false, blink_j, blink_rel);
			last_blink_ns = now;
		}

		/* FAULT_N1, at most one per 20 ms. */
		pthread_mutex_lock(&lock);
		if (fault_q_len > 0 && now - last_fault_ns >= MIN_EVENT_GAP_NS) {
			uint8_t code = fault_q[0].code, detail = fault_q[0].detail;
			uint64_t fault_t = fault_q[0].t;

			memmove(&fault_q[0], &fault_q[1], (size_t)(fault_q_len - 1) * sizeof(fault_q[0]));
			fault_q_len--;
			pthread_mutex_unlock(&lock);
			send_fault(code, detail, fault_t);
			last_fault_ns = now;
		} else {
			pthread_mutex_unlock(&lock);
		}
	}
	return NULL;
}

/* ===================================================================== */
/* Report and commands                                                   */
/* ===================================================================== */

static void *report_thread(void *arg)
{
	uint64_t start = t0_ns, next = now_ns() + 1000 * MS;
	unsigned reports = 0;

	(void)arg;
	while (!atomic_load(&stop)) {
		sleep_until(next);
		next += 1000 * MS;

		uint64_t now = now_ns();
		unsigned n = atomic_exchange(&lat_n, 0);
		unsigned long long sum = atomic_exchange(&lat_sum_ns, 0);
		unsigned long long mx = atomic_exchange(&lat_max_ns, 0);

		pthread_mutex_lock(&lock);
		enum zone_state z = zone;
		uint8_t lf = local_fault;
		bool alive2 = peer_alive[NODE_N2], alive3 = peer_alive[NODE_N3];
		bool f2 = peer_fault[NODE_N2], f3 = peer_fault[NODE_N3];
		uint8_t c2 = peer_fault_code[NODE_N2], c3 = peer_fault_code[NODE_N3];
		bool d[NUM_BUSES] = {deg[BUS_A], deg[BUS_B]};
		struct ctrl_cmd c = last_cmd;
		int32_t raw[3] = {last_raw[0], last_raw[1], last_raw[2]};
		uint64_t udp_age = have_udp ? (now - last_udp_ns) / MS : 0;
		bool hu = have_udp;
		enum blink_mode bm = z == ZONE_NORMAL ? selected : BLINK_HAZARD;
		pthread_mutex_unlock(&lock);

		if (opt.quiet) {
			continue;
		}
		printf("[N1 %.1fs] %s | peers: N2 %s%s N3 %s%s | local fault 0x%02x%s\n",
		       (double)(now - start) / 1e9, zone_names[z], alive2 ? "ok" : "LOST",
		       f2 ? " (fault)" : "", alive3 ? "ok" : "LOST", f3 ? " (fault)" : "", lf,
		       lf == FAULT_TEST_BUTTON ? " test button" : (lf == FAULT_UDP_TIMEOUT ? " no wheel data" : ""));
		if (f2 || f3) {
			printf("  peer fault codes: N2 0x%02x N3 0x%02x\n", c2, c3);
		}
		if (hu) {
			unsigned over = atomic_exchange(&udp_gaps_over, 0);

			printf("  wheel: %u pkt/s, last %llu ms ago, longest gap %llu ms (all-time %llu ms)%s\n",
			       atomic_exchange(&udp_count, 0), (unsigned long long)udp_age,
			       atomic_exchange(&udp_gap_max_ns, 0) / MS, atomic_load(&udp_gap_all_max_ns) / MS,
			       over ? " <- over the 90 ms timeout" : "");
			printf("  CTRL sent %u | raw steer %d thr %d brk %d -> steer %d thr %u brk %u btn 0x%x | blink %s\n",
			       atomic_exchange(&ctrl_sent, 0), raw[0], raw[1], raw[2], c.steer,
			       c.throttle, c.brake, c.buttons, blink_names[bm]);
		} else {
			printf("  wheel: no packets yet (is the proxy sending to UDP port %d?) | blink %s\n",
			       opt.udp_port, blink_names[bm]);
		}
		if (n) {
			printf("  UDP -> CAN write latency: mean %llu us, max %llu us (all-time max %llu us)\n",
			       sum / n / 1000, mx / 1000, atomic_load(&lat_all_max_ns) / 1000);
		}
		for (int bus = 0; bus < NUM_BUSES; bus++) {
			const char *st = if_state(bus);

			if (st[0] == 'D') {
				printf("  *** %s is DOWN: run ./can_up.sh (the cockpit can't send or receive on it) ***\n",
				       opt.ifname[bus]);
			}
			printf("  %c %s (%s): DEG %d | tx ok %u err %u skip %u | error frames %u | rx:", 'A' + bus,
			       opt.ifname[bus], st, d[bus], atomic_exchange(&tx_ok[bus], 0),
			       atomic_exchange(&tx_err[bus], 0), atomic_exchange(&tx_skip[bus], 0),
			       atomic_exchange(&err_frames[bus], 0));
			bool any = false;

			for (size_t s = 0; s < NUM_SLOTS; s++) {
				unsigned k = atomic_exchange(&rx_count[bus][s], 0);

				if (k) {
					printf(" 0x%03x x%u", catalog_ids[s], k);
					any = true;
				}
			}
			printf("%s\n", any ? "" : " (nothing)");
		}
		printf("  dup %u | FAULT rx %u | force %d, forwarded %u\n", atomic_exchange(&dup_count, 0),
		       atomic_exchange(&fault_rx, 0), atomic_load(&force_last),
		       atomic_exchange(&force_fwd, 0));
		if (++reports % J_REPORT_EVERY == 0) {
			printf("  J, trigger -> write() returned, us: max since start / max last %d s / mean (frames)\n",
			       J_REPORT_EVERY);
			for (int m = 0; m < J_NUM; m++) {
				bool any = false;

				for (int bus = 0; bus < NUM_BUSES; bus++) {
					unsigned k = atomic_load(&j_n[m][bus]);
					unsigned long long win = atomic_exchange(&j_max_win[m][bus], 0);

					if (k == 0) {
						continue;
					}
					if (!any) {
						printf("    %s", j_names[m]);
						any = true;
					}
					printf("  %c %.1f / %.1f / %.1f (%u)", 'A' + bus,
					       atomic_load(&j_max[m][bus]) / 1e3, win / 1e3,
					       atomic_load(&j_sum[m][bus]) / 1e3 / k, k);
				}
				if (any) {
					printf("\n");
				}
			}
		}
		fflush(stdout);
	}
	return NULL;
}

static void *stdin_thread(void *arg)
{
	char line[64];

	(void)arg;
	while (!atomic_load(&stop) && fgets(line, sizeof(line), stdin)) {
		switch (line[0]) {
		case 'm':
			atomic_store(&inject_malformed, true);
			printf("next CTRL: bad CRC\n");
			break;
		case 'r':
			atomic_store(&inject_range, true);
			printf("next CTRL: steering out of range\n");
			break;
		case 't':
			pthread_mutex_lock(&lock);
			if (!test_fault) {
				test_fault_ns = now_ns();
			}
			test_fault = true;
			pthread_mutex_unlock(&lock);
			printf("cockpit test fault on\n");
			break;
		case 'c':
			pthread_mutex_lock(&lock);
			test_fault = false;
			pthread_mutex_unlock(&lock);
			printf("cockpit test fault cleared\n");
			break;
		case 'q':
			kill(getpid(), SIGTERM); /* same path as Ctrl-C */
			break;
		default:
			printf("commands: m = next CTRL bad CRC, r = next CTRL out of range, "
			       "t = test fault on, c = clear it, q = quit\n");
		}
		fflush(stdout);
	}
	return NULL;
}

static void on_signal(int sig)
{
	if (sig == SIGUSR1) {
		atomic_store(&inject_malformed, true);
	} else if (sig == SIGUSR2) {
		atomic_store(&inject_range, true);
	} else if (atomic_exchange(&stop, true)) {
		/* Second Ctrl-C: leave now. */
		tp_all_low();
		_exit(130);
	}
}

/* ===================================================================== */
/* main                                                                  */
/* ===================================================================== */

static void set_rt(pthread_t t, int prio)
{
	if (opt.rt_prio > 0) {
		struct sched_param sp = {.sched_priority = prio};
		int err = pthread_setschedparam(t, SCHED_FIFO, &sp);

		if (err) {
			fprintf(stderr, "SCHED_FIFO %d: %s (run with sudo)\n", prio, strerror(err));
		}
	}
	if (opt.cpu >= 0) {
		cpu_set_t set;

		CPU_ZERO(&set);
		CPU_SET(opt.cpu, &set);
		pthread_setaffinity_np(t, sizeof(set), &set);
	}
}

static void usage(const char *prog)
{
	printf("usage: %s [options]\n"
	       "  --port N          UDP port for wheel packets (default 8765)\n"
	       "  --bind ADDR       local address to listen on (default 0.0.0.0)\n"
	       "  --force-host IP   where FORCE goes (default: the wheel packets' sender)\n"
	       "  --force-port N    proxy's force port (default 8001)\n"
	       "  --can-a IF        bus A interface (default: the one on SPI0 CE0, else can0)\n"
	       "  --can-b IF        bus B interface (default: the one on SPI0 CE1, else can1)\n"
	       "  --bitrate N       used when re-raising a flushed interface (default 500000)\n"
	       "  --rt PRIO         SCHED_FIFO: UDP thread PRIO, CAN rx PRIO-1, timer PRIO-2 (sudo)\n"
	       "  --cpu N           pin all threads to CPU N\n"
	       "  --flush           bounce an interface (ip link down/up, needs root) when its bus\n"
	       "                    goes degraded, to drop stale queued frames (off by default)\n"
	       "  --quiet           no once-a-second summary\n"
	       "While running, type m / r (corrupt the next CTRL), t / c (test fault on / off), q.\n"
	       "Signals: SIGUSR1 = next CTRL bad CRC, SIGUSR2 = next CTRL out of range.\n",
	       prog);
}

int main(int argc, char **argv)
{
	t0_ns = now_ns();
	static const struct option longopts[] = {
		{"port", required_argument, 0, 'p'},   {"bind", required_argument, 0, 'b'},
		{"force-host", required_argument, 0, 'H'}, {"force-port", required_argument, 0, 'P'},
		{"can-a", required_argument, 0, 'A'},  {"can-b", required_argument, 0, 'B'},
		{"bitrate", required_argument, 0, 'r'}, {"rt", required_argument, 0, 'R'},
		{"cpu", required_argument, 0, 'c'},    {"no-flush", no_argument, 0, 'n'},
		{"flush", no_argument, 0, 'f'},
		{"quiet", no_argument, 0, 'q'},        {"help", no_argument, 0, 'h'},
		{0, 0, 0, 0}};
	int ch;

	while ((ch = getopt_long(argc, argv, "h", longopts, NULL)) != -1) {
		switch (ch) {
		case 'p': opt.udp_port = atoi(optarg); break;
		case 'b': opt.bind_addr = optarg; break;
		case 'H': opt.force_host = optarg; break;
		case 'P': opt.force_port = atoi(optarg); break;
		case 'A': opt.ifname[BUS_A] = optarg; opt.if_given[BUS_A] = true; break;
		case 'B': opt.ifname[BUS_B] = optarg; opt.if_given[BUS_B] = true; break;
		case 'r': opt.bitrate = atoi(optarg); break;
		case 'R': opt.rt_prio = atoi(optarg); break;
		case 'c': opt.cpu = atoi(optarg); break;
		case 'n': opt.flush = false; break; /* default now; kept for old command lines */
		case 'f': opt.flush = true; break;
		case 'q': opt.quiet = true; break;
		default: usage(argv[0]); return ch == 'h' ? 0 : 2;
		}
	}

	setvbuf(stdout, NULL, _IOLBF, 0);
	if (tp_init() < 0) {
		perror("test points: /dev/gpiomem (continuing without them)");
	}
	tp_set(TP_ERR, 1);
	tp_set(TP_DEG_A, 1);
	tp_set(TP_DEG_B, 1);

	if (!getenv("COCKPIT_TEST_FD_A")) {
		resolve_interfaces();
	}
	for (int bus = 0; bus < NUM_BUSES; bus++) {
		/*
		 * Test hook (test_cockpit.py): use an inherited socket pair instead
		 * of a CAN interface, so the program can be exercised without CAN
		 * hardware. Unset in normal use.
		 */
		const char *test_fd = getenv(bus == BUS_A ? "COCKPIT_TEST_FD_A" : "COCKPIT_TEST_FD_B");

		if (test_fd) {
			can_fd[bus] = atoi(test_fd);
			test_mode = true;
			fcntl(can_fd[bus], F_SETFL, fcntl(can_fd[bus], F_GETFL) | O_NONBLOCK);
			continue;
		}
		can_fd[bus] = can_open(opt.ifname[bus]);
		if (can_fd[bus] < 0) {
			fprintf(stderr, "%s: %s (run ./can_up.sh first)\n", opt.ifname[bus], strerror(errno));
			return 1;
		}
	}

	for (int bus = 0; bus < NUM_BUSES; bus++) {
		if (if_state(bus)[0] == 'D') {
			fprintf(stderr, "%s is down: run ./can_up.sh first\n", opt.ifname[bus]);
			return 1;
		}
	}

	udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons((uint16_t)opt.udp_port)};

	addr.sin_addr.s_addr = inet_addr(opt.bind_addr);
	if (udp_fd < 0 || bind(udp_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("UDP bind");
		return 1;
	}
	/* Wake up at least every 200 ms with no wheel data, so a stop request is
	 * seen even if no packet ever arrives. */
	struct timeval rcv_timeout = {.tv_sec = 0, .tv_usec = 200000};

	setsockopt(udp_fd, SOL_SOCKET, SO_RCVTIMEO, &rcv_timeout, sizeof(rcv_timeout));
	if (opt.force_host) {
		proxy_addr.sin_family = AF_INET;
		proxy_addr.sin_port = htons((uint16_t)opt.force_port);
		proxy_addr.sin_addr.s_addr = inet_addr(opt.force_host);
		have_proxy = true;
	}

	struct sigaction sa = {.sa_handler = on_signal}; /* no SA_RESTART: recvfrom returns */

	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sa.sa_flags = SA_RESTART;
	sigaction(SIGUSR1, &sa, NULL);
	sigaction(SIGUSR2, &sa, NULL);

	if (opt.rt_prio > 0 && mlockall(MCL_CURRENT | MCL_FUTURE) < 0) {
		perror("mlockall");
	}

	pthread_t t_rx, t_timer, t_report, t_stdin;
	sigset_t worker_mask, old_mask;

	/*
	 * Signals go to the main (UDP) thread only: the workers start with
	 * SIGINT/SIGTERM/SIGUSR1/SIGUSR2 blocked, so Ctrl-C always interrupts the
	 * UDP read instead of landing on a thread that ignores it.
	 */
	sigemptyset(&worker_mask);
	sigaddset(&worker_mask, SIGINT);
	sigaddset(&worker_mask, SIGTERM);
	sigaddset(&worker_mask, SIGUSR1);
	sigaddset(&worker_mask, SIGUSR2);
	pthread_sigmask(SIG_BLOCK, &worker_mask, &old_mask);

	pthread_create(&t_rx, NULL, can_rx_thread, NULL);
	pthread_create(&t_timer, NULL, timer_thread, NULL);
	pthread_create(&t_report, NULL, report_thread, NULL);
	set_rt(pthread_self(), opt.rt_prio);
	set_rt(t_rx, opt.rt_prio - 1);
	set_rt(t_timer, opt.rt_prio - 2);
	if (opt.cpu >= 0) {
		set_rt(t_report, 0); /* pin only */
	}
	if (isatty(STDIN_FILENO)) {
		pthread_create(&t_stdin, NULL, stdin_thread, NULL);
		pthread_detach(t_stdin);
	}
	pthread_sigmask(SIG_SETMASK, &old_mask, NULL); /* main thread takes the signals */

	printf("cockpit: wheel packets on UDP %s:%d, CAN %s (A) / %s (B)%s. Type h for commands.\n",
	       opt.bind_addr, opt.udp_port, opt.ifname[BUS_A], opt.ifname[BUS_B],
	       opt.rt_prio > 0 ? ", SCHED_FIFO" : "");
	udp_loop();

	atomic_store(&stop, true);
	tp_all_low();
	printf("cockpit stopped\n");
	fflush(stdout);
	_exit(0); /* don't wait on worker threads blocked in poll/read/fgets */
}
