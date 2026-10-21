# N1 - Raspberry Pi: cockpit zone

`cockpit` is the Lab 3 cockpit program (C, SocketCAN). It takes the wheel's UDP
stream from the Mac proxy and drives both CAN buses:

- **CTRL on every wheel packet** (R4.4), written to can0 then can1 right away.
  Handout template layout with CRC-8 and Lab 2's wheel values (`catalog_pi.h`).
  Nothing else happens before the two writes; that path is the Pi's share of
  the 2 ms brake chain.
- **BLINK master**: buttons 4 (right) / 5 (left) as in Lab 2, cancelled after
  the turn (steering past +-5000, back inside +-4000). Sent on every change
  and toggled every 500 ms. Hazards whenever the cockpit isn't NORMAL.
- **HB_N1 every 20 ms** (Lab 1 layout: state, LOCAL_FAULT, degraded/bus-off bits).
- **HB_MON**: DEG_A/B after 44 ms without another zone on that bus, a zone
  lost after 66 ms on both buses, the zone state and ERR. A bus that goes
  degraded is skipped for everything but heartbeats. (`--flush` additionally
  bounces the interface with `ip link` to drop stale queued frames; off by
  default, since a failed bounce leaves the interface down.)
- **Faults**: wheel button 10 = cockpit test button (single press fails the
  zone, double press recovers); no wheel packet for 90 ms = UDP_TIMEOUT. Both
  send FAULT_N1 and set LOCAL_FAULT in HB_N1. A FAULT or LOCAL_FAULT from N2/N3
  puts the cockpit in SYS_ERROR until that zone's heartbeat clears it.
- **FORCE** from N2 is forwarded to the proxy over UDP (port 8001 of whoever
  sends the wheel packets, as in Lab 2).
- **Bad-input injection**: type `r` (next CTRL out of range) or `m` (next CTRL
  bad CRC), or send SIGUSR2 / SIGUSR1. A wheel value outside Lab 2's range
  (the proxy's fault button) also goes out as out-of-range steering.
- **Test points** (`testpoints.h`): UDP_RX on each wheel packet, CMD_TX right
  after the CTRL write to bus A, CAN_TX_A/B on every frame written, DEG_A/B, ERR.

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

## Build and run

```
make
./can_up.sh                       # both interfaces up at 500 kbit/s
./cockpit                         # refuses to start if can0/can1 are down
sudo ./cockpit --rt 80 --cpu 3    # SCHED_FIFO + pinned to core 3, for latency runs
./cockpit --help                  # ports, interfaces, options
```

On the Mac, run the proxy as in Lab 2, pointed at the Pi:
`python3 mac/mac_proxy.py --remote <pi-ip> --listen <mac-ip> --interval-ms 20`.

While it runs, type `m` / `r` (corrupt the next CTRL), `t` / `c` (cockpit test
fault on / off), `q` to quit. Once a second it prints:

```
[N1 12.0s] NORMAL | peers: N2 ok N3 ok | local fault 0x00
  wheel: 50 pkt/s, last 3 ms ago | CTRL sent 50 | raw steer 1200 thr 16384 brk 0 -> steer 1200 thr 128 brk 0 btn 0x0 | blink off
  UDP -> CAN write latency: mean 60 us, max 140 us (all-time max 310 us)
  A can0: DEG 0 | tx ok 110 err 0 skip 0 | error frames 0 | rx: 0x101 x50 0x102 x100 0x300 x20
  B can1: DEG 0 | tx ok 110 err 0 skip 0 | error frames 0 | rx: 0x101 x50 0x102 x50 0x300 x20
  dup 150 | FAULT rx 0 | force 23, forwarded 20
```

The latency line is the software time from the wheel packet arriving to both
CTRL writes returning; the report's Pi-stage number comes from the scope
(UDP_RX to CMD_TX), with and without load.

If the summary prints `*** can0 is DOWN ***`, or every peer shows `LOST`
while the Nucleos are running, run `./can_up.sh` again.

**can0 / can1 aren't fixed.** Linux names the two modules in the order they
probe, which can change between boots. Bus A is the module on SPI0 CE0 and
bus B the one on CE1, so `cockpit`, `can_test.py` and `can_up.sh` look the
interfaces up by chip-select and print `bus A = can1 (SPI0 CE0)` etc. at
start. When you use `candump` or `cansend` by hand, check that line (or
`readlink /sys/class/net/can0/device`) to know which interface is which bus.

## Testing without CAN hardware

`python3 test_cockpit.py` runs `./cockpit` against two simulated buses (socket
pairs) and plays N2 and N3 plus the proxy: 25 checks covering CTRL contents and
CRC, the state machine, BLINK timing, turn-signal cancel, fault injection,
FAULT handling, FORCE forwarding, a degraded bus, the test button and the
wheel timeout. Runs on any Linux machine, no root.

## Other tools here

- `can_test.py`: bring-up test (heartbeats only, Python) from Part 1.
- `testpoints.py`: walks every test point high in turn to check the wiring.
- `catalog.py` / `catalog_pi.h`: the message catalog in Python and C.

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
