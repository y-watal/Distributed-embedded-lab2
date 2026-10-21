# N2 - front Nucleo (steering): zone controller

The Lab 3 firmware for this zone: the shared dual-CAN layer, heartbeats, heartbeat
monitoring with degraded LEDs and zone failure, the always-on relay, the test
points, and the Lab 2 steering servo and servo current sensor.
Design decisions (catalog, thresholds, priorities, relay): `docs/lab3_design.md`.

Files listed under "shared" in `CMakeLists.txt` are identical in
`node2_front/` and `node3_rear/`; `src/node_config.h` (identity, relay,
current-sensor channels) and the zone's own actuator files differ.

| File | What it does |
|---|---|
| `src/catalog.h` | Message IDs, payload layouts, CTRL range check (Lab 2 values) |
| `src/lab3_config.h` | All thresholds and thread priorities |
| `src/canbus.c` | Both controllers, catalog filters, send-on-both-buses helper with per-ID sequence numbers, dedup by (ID, seq), heartbeat ages per bus |
| `src/hb.c` | HB_TX every 20 ms; HB_MON every 5 ms: DEG_A/B, zone failure, ERR |
| `src/ctrl.c` | Latest valid CTRL (CRC and range checked), wakes the actuator task |
| `src/current_sensor.c` | Lab 2 current sensors; channels from `node_config.h` (N2: servo) |
| `src/servo.c` | Lab 2 servo driver (PB4, TIM3) |
| `src/steer.c` | STEER: CTRL -> servo angle; centers unless NORMAL and CTRL fresh |
| `src/relay.c` | N2 only: forwards the first copy of every N1/N3 frame to the other bus |
| `src/report.c` | Console summary once a second |
| `src/tp.c` | Test points (pins in `app.overlay`, map in `docs/lab3_pin_map.md`) |

**Lab 2 hardware and test points:** wiring tables in `docs/lab3_pin_map.md`
(the Lab 2 actuators and sensors keep their Lab 2 pins; the blinker LEDs moved).

## Wiring (Nucleo Arduino header -> XIAO CAN module)

| Module pin | Function | CAN A module | CAN B module |
|---|---|---|---|
| 3V3 / GND | Power (3.3 V only) | 3V3 / GND | 3V3 / GND |
| D8 | SCK | D13 (PA5) | D13 (PA5) |
| D10 | MOSI | D11 (PA7) | D11 (PA7) |
| D9 | MISO | D12 (PA6) | D12 (PA6) |
| D7 | CS | D10 (PB6) | D9 (PC7) |
| D6 | INT | D2 (PA10) | D4 (PB5) |

The Lab 3 handout's table has MOSI/MISO swapped on the module side; the
XIAO footprint is D9 = MISO, D10 = MOSI. If you change a pin, change
`app.overlay` to match. Check `osc-freq` against the crystal on the module.

## Build and flash

```
west build -p always -b nucleo_f401re node2_front -d node2_front/build
west flash -d node2_front/build
```

With both Nucleos plugged into the same laptop, pick the board by its
ST-LINK serial: `west flash -d node2_front/build --dev-id <serial>`
(list serials with `st-info --probe` or `STM32_Programmer_CLI -l`).

## Console (ST-LINK virtual COM port, 115200 baud)

At boot every test point goes high for 300 ms in turn (set `TP_WALK_MS` to 0
in `src/lab3_config.h` to skip). Then, once a second, with all three nodes up:

```
[N2 12.000s] NORMAL | peers: N1 ok N3 ok | dup 100 echo 0 | relay 100 skip 0 drop 0
  A: error-active tec=0 rec=0 DEG 0 | tx ok=50 err=0 full=0 skip=0 | rx: 0x100 x50 0x102 x50
  B: error-active tec=0 rec=0 DEG 0 | tx ok=150 err=0 full=0 skip=0 | rx: 0x100 x50 0x102 x50
  accepted: 0x100 x50 0x102 x50
  ctrl: ok 50 malformed 0 range 0 | last: steer 1200 throttle 0 brake 0 buttons 0x0, 4 ms ago
  steer: following angle 100 deg | servo 85 mA
```

(Example assumes N2 usually gets the bus A copy first, so its forwards land on bus B; the split between buses can differ, the `accepted` line can't.)

- `steer`: `following` while NORMAL with fresh CTRL, else `centered` (90 deg).
- The `ctrl` line counts CTRL frames accepted / rejected (bad CRC or reserved byte
  = malformed, steering or button bits out of range = range) and shows the last one.
- `0x101` is this node's heartbeat; `peers` turn `LOST` 66 ms after a zone's
  heartbeat stops on both buses, and the state goes `SYS_ERROR` (ERR high).
- `rx` counts every copy on that bus; `accepted` counts each (ID, seq) once.
  `dup` is copies dropped, `echo` is this node's own frames relayed back by N2.
- `relay` is frames N2 forwarded to the other bus (about 100/s: N1's and N3's
  heartbeats, one forward each); `tx ok` includes them. `skip` means the
  destination bus is degraded.
- `DEG 1` (LED on): no other node heard on that bus for 44 ms. Pull one of
  this node's CANH/CANL jumpers and that bus's DEG comes on, `tx full` climbs on
  it, and everything keeps running on the other bus.
- `not ready` at boot: SPI wiring, CS pin, or no 3V3 to the module.
- IDs at x25 or x100 instead of x50 in `accepted`: an `osc-freq`/`oscillator=` mismatch.

## Test points driven now

CAN_TX_A/B (every queued frame, including relayed ones), CAN_RX_A/B (every received copy), DEG_A/B, ERR, RELAY_TX, and SRV (the servo PWM). FL and FR are only driven by the boot walk until BLINK is implemented.
