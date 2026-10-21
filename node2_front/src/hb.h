/*
 * HB_TX: this node's heartbeat every HB_PERIOD_MS on both buses.
 * HB_MON: every MONITOR_PERIOD_MS, per-bus degraded state (DEG LEDs, R4.7) and
 * per-zone failure (R4.5) from the heartbeat ages kept by canbus.
 */
#ifndef HB_H
#define HB_H

#include <stdbool.h>

#include "catalog.h"

int hb_init(void);

bool hb_bus_degraded(int bus);
bool hb_peer_alive(int node);
enum zone_state hb_zone_state(void);
const char *hb_state_name(enum zone_state state);

/* Set by the relay so the heartbeat carries RELAY_ACTIVE. */
void hb_set_relay_active(bool active);

#endif /* HB_H */
