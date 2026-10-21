/*
 * Every timing threshold and thread priority in one place (handout Part 5).
 * Derivations: docs/lab3_design.md.
 */
#ifndef LAB3_CONFIG_H
#define LAB3_CONFIG_H

/* Heartbeats (R4.5): every 20 ms +-10 %. */
#define HB_PERIOD_MS       20
#define HB_MAX_INTERVAL_MS 22

/*
 * A bus is degraded at this node when no other node's heartbeat has arrived on
 * it for DEG_TIMEOUT_MS. Worst-case detection is DEG_TIMEOUT_MS + MONITOR_PERIOD_MS
 * = 49 ms, inside R4.7's 60 ms. Two intervals keeps it clear of the longest
 * healthy gap (~24 ms), so the LED doesn't flicker.
 */
#define DEG_TIMEOUT_MS (2 * HB_MAX_INTERVAL_MS)

/*
 * A zone has failed when its heartbeat has been missing on both buses for
 * FAIL_TIMEOUT_MS: three missed heartbeats, each allowed its 22 ms window.
 * Worst case 66 + 5 = 71 ms, leaving 29 ms of the 100 ms system-error budget.
 */
#define FAIL_TIMEOUT_MS (3 * HB_MAX_INTERVAL_MS)

#define MONITOR_PERIOD_MS 5

/*
 * Copies of one (ID, seq) - bus A, bus B, relayed - all arrive within a few ms.
 * A repeat of the same seq inside this window is a duplicate; outside it, the
 * 4-bit counter has wrapped and it's a new frame. Must stay below the shortest
 * interval between two frames of one ID (18 ms).
 */
#define DUP_WINDOW_MS 10

#define REPORT_PERIOD_MS 1000

/*
 * CTRL older than this is stale: actuators go to their safe state (brake /
 * center). Lab 2's 90 ms: four missed 20 ms CTRL frames, plus margin.
 */
#define CTRL_TIMEOUT_MS 90

/* Actuator tasks re-check CTRL freshness and zone state at least this often. */
#define ACTUATOR_POLL_MS 5

#define PID_PERIOD_MS     10 /* N3 speed loop (template: PID every 10 ms) */
#define CURRENT_PERIOD_MS 20 /* current sensor sampling */

/* Boot-time test point walk, ms per point; 0 skips it. */
#define TP_WALK_MS 300

/*
 * Thread priorities (lower number = higher priority). The MCP2515 driver's two
 * interrupt threads run at CONFIG_CAN_MCP2515_INT_THREAD_PRIO (2), above all of these.
 * RELAY sits above DISPATCH so a forwarded CTRL goes out before N2's own tasks
 * see the frame: it is on N3's brake path.
 */
#define PRIO_RELAY    3
#define PRIO_DISPATCH 4
#define PRIO_DRIVE    5 /* N3: acts on CTRL (brake path, local deadline 0.5 ms) */
#define PRIO_STEER    5 /* N2: acts on CTRL (local deadline 1 ms) */
#define PRIO_PID      6 /* N3: speed loop every 10 ms */
#define PRIO_HB_TX    7
#define PRIO_HB_MON   8
#define PRIO_CURRENT  10
#define PRIO_REPORT   12

#endif /* LAB3_CONFIG_H */
