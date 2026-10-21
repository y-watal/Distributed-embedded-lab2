# Distributed-embedded-lab2

Drive-by-wire tabletop car, 18-649. Lab 3: dual CAN bus between the cockpit Pi
and two STM32 Nucleo-F401RE zone controllers.

## Layout

| Folder | Node | What it is |
|---|---|---|
| `node1_pi/` | N1, Raspberry Pi 4 (cockpit) | `cockpit` (C, SocketCAN): wheel UDP -> CTRL/BLINK on both buses, HB_N1, HB_MON, FAULT, FORCE |
| `node2_front/` | N2, front Nucleo | Zephyr app, two MCP2515 on SPI1 |
| `node3_rear/` | N3, rear Nucleo | Zephyr app, two MCP2515 on SPI1 |
| `mac/` | Laptop | Wheel/pedal UDP proxy (from Lab 2) |
| `lab2_backup/` | - | Complete Lab 2 code: Zephyr app (`west build -b nucleo_f401re lab2_backup`) and the Pi UART receiver in `lab2_backup/pi/` |
| `docs/lab3_pin_map.md` | - | Lab 3 pins: CAN modules and every test point, per node |
| `docs/lab3_design.md` | - | Lab 3 catalog, relay policy, thresholds, thread priorities |
| `docs/`, `schematic/` | - | Lab 2 documentation and schematic |

## Current stage

All the software for the checkoff sequence is written (not yet run on the
hardware):

- **Nucleos**: dual-CAN layer (heartbeats every 20 ms, dedup by (ID, seq), DEG
  LEDs after 44 ms, zone lost after 66 ms), N2's always-on relay, CTRL checking
  (CRC-8, ranges), the zone state machine and fault protocol, test button B1,
  blinkers following the cockpit, FORCE (N2), the watchdog, and the Lab 2
  hardware: steering servo (N2), H-bridge + encoders + PID (N3), current sensors.
- **Pi**: the cockpit program (`node1_pi/cockpit.c`) and its hardware-free
  test (`node1_pi/test_cockpit.py`).

Decisions, thresholds, the fault protocol and a checkoff walk-through:
`docs/lab3_design.md`. Wiring: `docs/lab3_pin_map.md`.
