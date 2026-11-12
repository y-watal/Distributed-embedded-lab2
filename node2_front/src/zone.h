/*
 * Zone state machine and fault protocol (docs/lab3_design.md, "Zone state and
 * fault protocol"; the cockpit implements the same rules).
 *
 *   BOOT       until every other zone's heartbeat has been heard and a valid
 *              CTRL is fresh
 *   ISOLATED   (after boot) no other zone heard on either bus: fail-safe
 *   SYS_ERROR  a local fault, another zone lost, or another zone reporting a
 *              fault or not ready (BOOT / ISOLATED)
 *   NORMAL     otherwise; recovery is automatic, no reset
 *
 * Local faults, highest first: TEST_BUTTON (B1 single press; a double press
 * clears it), CTRL_RANGE / CTRL_MALFORMED (held CTRL_FAULT_HOLD_MS, then
 * cleared by the next valid CTRL), CTRL_TIMEOUT (no valid CTRL for
 * CTRL_TIMEOUT_MS after boot). Entering one sends FAULT_Nx once; while it
 * lasts every heartbeat carries LOCAL_FAULT and the code.
 */
#ifndef ZONE_H
#define ZONE_H

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/drivers/can.h>

#include "catalog.h"

/* Recomputes the state and sends a queued FAULT. HB_MON calls it every
 * MONITOR_PERIOD_MS; the test button calls it right away. */
void zone_update(void);

/* Dispatch thread, first copies of HB_Nx and FAULT_Nx from other zones. */
void zone_on_frame(const struct can_frame *frame);

/* Dispatch thread, from the CTRL check. */
void zone_ctrl_rejected(enum fault_code code);
void zone_ctrl_accepted(void);

/* TEST_BTN thread, one debounced press. */
void zone_test_button(uint32_t press_cyc); /* jit_now() at the button interrupt */

/* Reported in heartbeat byte 3 until a newer fault (e.g. WATCHDOG_RESET at boot). */
void zone_note_fault(enum fault_code code);

enum zone_state zone_state(void);
uint8_t zone_local_fault(void); /* 0 = none */
uint8_t zone_last_fault(void);  /* most recent local fault, for HB byte 3 */
bool zone_peer_fault(int node, uint8_t *code);
enum zone_state zone_peer_state(int node);
const char *zone_state_name(enum zone_state state);

#endif /* ZONE_H */
