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

/* Zone state machine (docs/lab3_design.md, "Zone state and fault protocol") */
#define TEST_BTN_DEBOUNCE_MS 50   /* template: TEST_BTN min interarrival 50 ms */
#define DOUBLE_PRESS_MS      350  /* second press within this clears the test fault */
#define CTRL_FAULT_HOLD_MS   1000 /* a rejected CTRL keeps the zone in error this long */
#define FAULT_CLEAR_HOLD_MS  (HB_MAX_INTERVAL_MS + 3) /* older HB may overtake a FAULT */
#define FAULT_MIN_GAP_MS     20   /* catalog: FAULT min interval */

/* Blinkers */
#define BLINK_HALF_MS  500  /* local hazard rate when the cockpit can't be heard (1 Hz) */
/*
 * No BLINK for this long while N1's heartbeat is still heard: flash hazards
 * locally. Losing the cockpit is detected by the heartbeat (66 ms); this is
 * only a backstop for a cockpit that is alive but has stopped sending BLINK,
 * so it tolerates 5 lost toggles. At 1100 ms (2 lost), a couple of frames
 * dropped on a single-path relay (one bus unplugged) put up false hazards.
 */
#define BLINK_STALE_MS (5 * BLINK_HALF_MS + 100)

/* FORCE_TX (N2): dummy counter every 50 ms */
#define FORCE_PERIOD_MS 50

/* Independent watchdog (R4.5): fed only while HB_TX and the actuator task check in. */
#define WDT_TIMEOUT_MS 250
#define WDT_CHECK_MS   100

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
#define PRIO_BLINK    5 /* BLINK_F / BLINK_R: 0.5 ms from reception */
#define PRIO_TEST_BTN 5 /* local fail within 10 ms (R2.5) */
#define PRIO_PID      6 /* N3: speed loop every 10 ms */
#define PRIO_HB_TX    7
#define PRIO_HB_MON   8 /* also runs the zone state machine and feeds the watchdog */
#define PRIO_FORCE    9 /* N2: FORCE every 50 ms */
#define PRIO_CURRENT  10
#define PRIO_REPORT   12
#define PRIO_EVLOG    13 /* event lines; below REPORT so it never splits a summary */

#endif /* LAB3_CONFIG_H */
