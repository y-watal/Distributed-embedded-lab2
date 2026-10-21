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

Done on the Nucleos: the dual-CAN layer (heartbeats every 20 ms, dedup by
(ID, seq), DEG LEDs after 44 ms, zone failure after 66 ms), N2's always-on
relay, CTRL checking (CRC-8, ranges), and the Lab 2 hardware: steering servo
on N2, H-bridge + encoders + PID on N3, current sensors on both. Decisions and
thresholds: `docs/lab3_design.md`; wiring: `docs/lab3_pin_map.md`.

Done on the Pi: the complete cockpit program (`node1_pi/cockpit.c`), with a
hardware-free test (`node1_pi/test_cockpit.py`).

Next on the Nucleos: blinkers from BLINK, the error state machine with test
buttons and FAULT messages (protocol in `docs/lab3_design.md`), the watchdog.
