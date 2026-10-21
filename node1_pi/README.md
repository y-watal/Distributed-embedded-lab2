# N1 - Raspberry Pi: CAN bring-up test

Sends `HB_N1` (`0x100`) every 100 ms on `can0` (bus A) and `can1` (bus B) and
prints a once-a-second receive summary.

## One-time setup

`/boot/firmware/config.txt` (comment out the Lab 2 `dtoverlay=uart3`), then reboot:

```
dtparam=spi=on
dtoverlay=mcp2515-can0,oscillator=16000000,interrupt=25
dtoverlay=mcp2515-can1,oscillator=16000000,interrupt=24
```

```
sudo apt install can-utils python3-can
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
./can_test.py          # or: ./can_test.py --listen-only
candump -c -t d any    # in a second terminal, raw view of both buses
```

Expected once both Nucleos are running:

```
[N1 12.0s]
  A can0: ERROR-ACTIVE | tx ok=10 err=0 | rx: 0x101 x10 0x102 x10
  B can1: ERROR-ACTIVE | tx ok=10 err=0 | rx: 0x101 x10 0x102 x10
```
