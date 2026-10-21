# N3 - rear Nucleo (drivetrain): zone controller

The Lab 3 firmware for this zone: the shared dual-CAN layer, heartbeats, heartbeat
monitoring with degraded LEDs and zone failure, the test points, and the Lab 2
drivetrain: H-bridge, encoders, PID speed loop and motor current sensors.
The PID loop is currently compiled out (`DRIVE_PID_ENABLED 0` in `src/drive.c`)
because the motors aren't connected: DRIVE sets the duty straight from the
target speed (Lab 2's feedforward, 550-700 /1000).
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
| `src/current_sensor.c` | Lab 2 current sensors; channels from `node_config.h` (N3: both motors) |
| `src/motor.c` | Lab 2 L298 H-bridge driver (PA0/PA1 PWM, PB12-15) |
| `src/encoder.c` | Lab 2 wheel encoders (TIM1, TIM2) |
| `src/drive.c` | DRIVE (CTRL -> brake / coast / duty from target speed) and PID (10 ms speed loop, compiled out for now) |
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
west build -p always -b nucleo_f401re node3_rear -d node3_rear/build
west flash -d node3_rear/build
```

With both Nucleos plugged into the same laptop, pick the board by its
ST-LINK serial: `west flash -d node3_rear/build --dev-id <serial>`
(list serials with `st-info --probe` or `STM32_Programmer_CLI -l`).

## Console (ST-LINK virtual COM port, 115200 baud)

At boot every test point goes high for 300 ms in turn (set `TP_WALK_MS` to 0
in `src/lab3_config.h` to skip). Then, once a second, with all three nodes up:

```
[N3 12.000s] NORMAL | peers: N1 NORMAL N2 NORMAL | dup 150 echo 50
  A: error-active tec=0 rec=0 DEG 0 | tx ok=50 err=0 full=0 skip=0 | rx: 0x100 x50 0x101 x50
  B: error-active tec=0 rec=0 DEG 0 | tx ok=50 err=0 full=0 skip=0 | rx: 0x100 x100 0x101 x50 0x102 x50
  accepted: 0x100 x50 0x101 x50
  ctrl: ok 50 malformed 0 range 0 | last: steer 0 throttle 128 brake 0 buttons 0x0, 3 ms ago
  blink: off, lamps off
  drive: DRIVE target 327 mm/s (open loop, PID off) | duty L 625 R 625 /1000 | I L 410 mA | I R 395 mA
```

(Example assumes N2 usually gets the bus A copy first, so its forwards land on bus B; the split between buses can differ, the `accepted` line can't.)

- `drive`: OFF (dynamic brake: zone not NORMAL or CTRL older than 90 ms), BRAKE
  (brake >= 4), COAST (throttle <= 2) or DRIVE (PID when it's enabled). Motors only run once both other
  zones are heard and the Pi is sending CTRL.
- The `ctrl` line counts CTRL frames accepted / rejected (bad CRC or reserved byte
  = malformed, steering or button bits out of range = range) and shows the last one.
- State: BOOT until both other zones are heard and CTRL arrives; NORMAL;
  SYS_ERROR (ERR high) with a local fault, a lost zone or a faulted / not-ready
  zone; ISOLATED (fail-safe, local hazards) when no other zone is heard.
  `(local fault 0x01)` = test button, `0x04` / `0x05` = bad CTRL (range /
  CRC), `0x06` = no CTRL for 90 ms. Peers show their state, `fault 0x..` or `LOST`.
- **Test button**: the blue USER button B1. One press puts the zone in
  SYS_ERROR and sends a FAULT (motors brake, servo centers); press twice
  quickly to recover.
- `blink`: the lamps follow the cockpit's BLINK; `(local)` means hazards
  generated here because the cockpit can't be heard.
- Watchdog: 250 ms; if the board ever resets from it, the next boot prints
  `*** last reset was the watchdog ***`.
- `0x102` is this node's heartbeat; `peers` turn `LOST` 66 ms after a zone's
  heartbeat stops on both buses, and the state goes `SYS_ERROR` (ERR high).
- `rx` counts every copy on that bus; `accepted` counts each (ID, seq) once.
  `dup` is copies dropped, `echo` is this node's own frames relayed back by N2.
- N3 sees N1's frames twice per bus (N1's copy and N2's forward), and its own
  heartbeat comes back from N2 as `echo` (how many depends on which copy N2
  sees first).
- `DEG 1` (LED on): no other node heard on that bus for 44 ms. Pull one of
  this node's CANH/CANL jumpers and that bus's DEG comes on, `tx full` climbs on
  it, and everything keeps running on the other bus.
- `not ready` at boot: SPI wiring, CS pin, or no 3V3 to the module.
- IDs at x25 or x100 instead of x50 in `accepted`: an `osc-freq`/`oscillator=` mismatch.

## Test points driven now

All of this zone's rows: CAN_TX_A/B, CAN_RX_A/B, DEG_A/B, ERR, PWM_SET (every duty or brake set: once per CTRL that reaches the motor, plus the fail-safe brake), TARGET_CHG (a CTRL changed the target speed), PWM_OUT (left motor PWM), DIR_A (H-bridge IN1), and RL / RR (the rear blinkers).
