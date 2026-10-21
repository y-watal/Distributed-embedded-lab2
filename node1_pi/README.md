# N1 - Raspberry Pi: CAN bring-up test

Sends `HB_N1` (`0x100`) every 20 ms on `can0` (bus A) and `can1` (bus B) and
prints a once-a-second receive summary.

## One-time setup

`/boot/firmware/config.txt` (comment out the Lab 2 `dtoverlay=uart3`), then reboot:

```
dtparam=spi=on
dtoverlay=mcp2515-can0,oscillator=16000000,interrupt=25
dtoverlay=mcp2515-can1,oscillator=16000000,interrupt=24
```

```
sudo apt install can-utils python3-can python3-lgpio
dmesg | grep -i mcp251     # both controllers should probe
```

## Wiring (Pi header -> XIAO CAN module)

| Module pin | Function | CAN A module (can0) | CAN B module (can1) |
|---|---|---|---|
| 3V3 / GND | Power | pin 1 or 17 / GND | pin 1 or 17 / GND |
| D8 | SCK | GPIO11, pin 23 | GPIO11, pin 23 |
| D10 | MOSI | GPIO10, pin 19 | GPIO10, pin 19 |
| D9 | MISO | GPIO9, pin 21 | GPIO9, pin 21 |
| D7 | CS | GPIO8 / CE0, pin 24 | GPIO7 / CE1, pin 26 |
| D6 | INT | GPIO25, pin 22 | GPIO24, pin 18 |

GPIO24 was the Lab 2 "UDP RX" debug toggle in `lab2_backup/pi/uart_receiver.c`;
it is now the can1 interrupt, so move that debug pin before reusing the code.

## Run

```
./can_up.sh            # both interfaces up at 500 kbit/s, restart-ms 100
./can_test.py          # options: --listen-only, --no-walk, --period-ms N
candump -c -t d any    # in a second terminal, raw view of both buses
```

Expected once both Nucleos are running:

```
[N1 12.0s] ERR 0
  A can0: ERROR-ACTIVE DEG 0 | tx ok=50 err=0 | rx: 0x101 x50 0x102 x50
  B can1: ERROR-ACTIVE DEG 0 | tx ok=50 err=0 | rx: 0x101 x50 0x102 x50
```

## Test points

| Row | Signal | GPIO (header pin) |
|---:|---|---|
| 1, 2 | GND | pins 39, 34 |
| 3 | UDP_RX | GPIO5 (29) |
| 4 | CMD_TX | GPIO6 (31) |
| 5 | CAN_TX_A | GPIO13 (33) |
| 6 | CAN_TX_B | GPIO19 (35) |
| 7 | DEG_A (LED) | GPIO26 (37) |
| 8 | DEG_B (LED) | GPIO16 (36) |
| 9 | ERR (added) | GPIO20 (38) |

- `./testpoints.py` drives each point high for 300 ms in turn so you can check the wiring;
  `can_test.py` does the same at start-up unless given `--no-walk`.
- `testpoints.py` is the Python module (lgpio). `testpoints.h` is the same thing for C
  (`tp_init()`, `tp_toggle(TP_UDP_RX)`, `tp_set(TP_ERR, 1)`), using `/dev/gpiomem` like Lab 2,
  for the cockpit program.
- In `can_test.py`: CAN_TX_A/B toggle per send, DEG_A/B are on while N2 and N3 are both
  silent on that bus for 60 ms, and ERR is on while N2 or N3 is silent on both buses.
  UDP_RX and CMD_TX are only exercised by the walk until the cockpit code exists.
