# Distributed-embedded-lab2

Drive-by-wire tabletop car, 18-649. Lab 3: dual CAN bus between the cockpit Pi
and two STM32 Nucleo-F401RE zone controllers.

## Layout

| Folder | Node | What it is |
|---|---|---|
| `node1_pi/` | N1, Raspberry Pi 4 (cockpit) | SocketCAN test on `can0` (bus A) and `can1` (bus B) |
| `node2_front/` | N2, front Nucleo | Zephyr app, two MCP2515 on SPI1 |
| `node3_rear/` | N3, rear Nucleo | Zephyr app, two MCP2515 on SPI1 |
| `mac/` | Laptop | Wheel/pedal UDP proxy (from Lab 2) |
| `lab2_backup/` | - | Complete Lab 2 code: Zephyr app (`west build -b nucleo_f401re lab2_backup`) and the Pi UART receiver in `lab2_backup/pi/` |
| `docs/`, `schematic/` | - | Lab 2 documentation and schematic |

## Current stage: CAN bring-up test

Each node sends its heartbeat ID from the message catalog every 100 ms on
both buses (N1 `0x100`, N2 `0x101`, N3 `0x102`) and prints, once a second,
what it received per bus. The checkpoint passes when every node shows the
other two IDs at about x10 per second on both A and B, with no gaps.
