# Lab 3 design decisions

Decisions behind the code in `node1_pi/`, `node2_front/` and `node3_rear/`.
Thresholds and priorities live in `node2_front/src/lab3_config.h` (same file in
`node3_rear/`). Changes from the handout's template and from our Lab 1 design
need a Lab 1 revision-history entry.

## Message catalog

Standard 11-bit IDs, DLC 8, sent on both buses. Byte 0 of every payload: low
nibble = 4-bit sequence counter, **one counter per ID**, the same on both bus
copies; high nibble = flags (none defined, 0). Receivers drop repeats of an
(ID, seq) that arrive within 10 ms (the A copy, the B copy, N2's relayed copy).

| ID | Name | From -> To | Period / trigger | Bytes 1-7 |
|---|---|---|---|---|
| 0x010 | CTRL | N1 -> N2, N3 | every UDP update (20 ms) | steer int16 LE (1-2), throttle uint8 (3), brake uint8 (4), buttons (5), CRC-8 (6), reserved 0 (7) |
| 0x020 / 0x021 / 0x022 | FAULT_N1 / N2 / N3 | sender -> all | on fault, min 20 ms apart | fault code (1), detail (2) |
| 0x100 / 0x101 / 0x102 | HB_N1 / N2 / N3 | sender -> all | 20 ms +-10 % | state (1), bus bits (2), last fault code (3) |
| 0x200 | BLINK | N1 -> N2, N3 | on change + every 500 ms toggle | bit 0 left, bit 1 right (both = hazard), bit 2 phase |
| 0x300 | FORCE | N2 -> N1 | 50 ms | force int8 (dummy counter) |

**Changes from the handout template**

- **CTRL follows the handout template**, carrying Lab 2's wheel values:
  - steering: Lab 2's raw value, valid -32767..32766
  - throttle and brake: Lab 2's raw 0..32767 shifted right by 7 into 0..255.
    Lab 2's thresholds become: throttle deadband 300 raw -> 2, brake active
    500 raw -> 4 (512 raw)
  - buttons 4 / 5 / 10: bits 0 / 1 / 2 of byte 5, other bits must be 0
  - byte 6: **CRC-8/SAE-J1850** (polynomial 0x1D, init 0xFF, final XOR 0xFF,
    no reflection; check value "123456789" -> 0x4B) over bytes 0-5, so the
    sequence byte is covered. Byte 7 is reserved and must be 0
  - a bad CRC or a nonzero reserved byte is `FAULT_CTRL_MALFORMED`; steering
    out of range or a stray button bit is `FAULT_CTRL_RANGE`. Every 8-bit
    pedal value is valid, so range faults are injected through steering: the Pi
    sends a wheel value outside Lab 2's range as steer = -32768 instead of
    wrapping it (`ctrl_from_lab2()` in `node1_pi/catalog.py`)
  - C: `ctrl_encode()` / `ctrl_check()` in `catalog.h`; Python:
    `ctrl_payload()` / `ctrl_from_lab2()` in `node1_pi/catalog.py`. Both CRCs
    give identical results on the check value and test frames
- **FAULT has one ID per sender.** Two nodes sending the same ID with different
  data at the same time is a CAN bit error, not an arbitration loss; and the
  relay needs to know who sent a frame. All three stay above the heartbeats.

**HB, FAULT and BLINK fields follow Lab 1**: HB byte 1 = STATE (0 BOOT, 1 SYS_ERROR,
2 NORMAL, 3 ISOLATED) and LOCAL_FAULT (bit 4); byte 2 = A_DEGRADED, B_DEGRADED,
A_BUSOFF, B_BUSOFF, RELAY_ACTIVE (bits 0-4). FAULT codes are Lab 1's
ERR_NOTIFY codes (0x01 TEST_BUTTON ... 0x0A PEER_LOST).

## Relay: N2 always relays

N2 forwards the first copy of every frame from N1 and N3, bit-identical, to the
other bus, from its own RELAY thread (toggles RELAY_TX). No fault detection.

- **Why always**: in R4.8 scenario (c) (N1 off B, N3 off A) the forwarded copies
  are already flowing when the second jumper comes out, so there is no gap in
  CTRL to N3 and no race against N3's failure detection. One mode to analyze;
  the checkoff measurement matches the analysis.
- **Cost**: each bus carries up to two copies of every N1/N3 frame (about 2x
  load; Appendix A's 8.6 % becomes roughly 17 %), N2 does one extra SPI
  transmit per forwarded frame, and N1/N3 each receive extra copies they drop.
- **Loops**: impossible. Only N2 forwards, a controller never receives its own
  transmissions, and N2 forwards each (ID, seq) once.
- **Bit-identical copies**: no "relayed" flag in the payload, so N2's copy and
  the original can collide harmlessly on a bus (identical frames merge).
- **N2 skips a destination bus it sees as degraded** so its TX buffers don't
  fill with frames nobody can ACK.
- **Not covered**: cross faults that cut N1 from N2 (N3 would have to relay) or
  N2 from N3 (N1 would have to relay). Not in the checkoff sequence; the report
  enumerates them (Part 4.3 guiding question) and the Lab 1 fault matrix rows
  for them change from "fully operational" to the behavior below.

| Double fault | Relay needed from | Without it |
|---|---|---|
| N1 off one bus, N3 off the other | N2 (implemented) | - |
| N1 off one bus, N2 off the other | N3 | N2 loses CTRL/BLINK; N1 loses HB_N2/FORCE; both see each other as failed after 66 ms -> system error |
| N2 off one bus, N3 off the other | N1 | N2 and N3 lose each other's heartbeat -> system error after 66 ms |

## Timing thresholds

Assumptions: heartbeat interval at most 22 ms (20 ms + 10 %); HB_MON runs every
5 ms; the longest gap between two heartbeat arrivals on a healthy bus is about
24 ms (22 ms + queueing).

| Threshold | Value | Requirement it serves |
|---|---:|---|
| Degraded (DEG LED) | no other node heard on that bus for **44 ms** (2 x 22) | R4.7 / checkoff 5: on within 60 ms. Worst case 44 + 5 = 49 ms. Three missed (66 + 5 = 71 ms) would miss it. Above 24 ms, so no flicker |
| Zone failed | heartbeat missing on **both** buses for **66 ms** (3 x 22) | R4.5. Worst case 66 + 5 = 71 ms, leaving 29 ms of checkoff 8's 100 ms |
| Duplicate window | **10 ms** | Copies arrive within a few ms; below the 18 ms minimum interval of one ID |
| Monitor period | **5 ms** | A 20 ms period would push DEG detection past 60 ms |

DEG clears on the first heartbeat heard again (R4.7: off within 100 ms of
restoring; worst case about 22 ms + bus-off recovery).

**Stale frames**: a controller cut off from its bus keeps retrying whatever is
in its TX buffers. When the link comes back, those go out first. To keep them
from being accepted as new (the 4-bit counter has wrapped many times), a node
aborts pending transmissions on a bus when it becomes degraded and before every
heartbeat while it stays degraded, and sends only heartbeats there. At most one
20 ms-old heartbeat is stale on restore.

## Threads on each STM32

Lower number = higher priority (`lab3_config.h`).

| Thread | Node | Priority | Job |
|---|---|---:|---|
| MCP2515 driver, one per controller | both | 2 | SPI frame read on INT; `rx_cb` toggles CAN_RX_A/B and queues the frame |
| relay | N2 | 3 | Forward first copies to the other bus, RELAY_TX. Above dispatch: it's on N3's brake path |
| dispatch | both | 4 | Dedup by (ID, seq), record heartbeat times per bus, check CTRL (CRC, range), store it and wake the actuator task |
| steer | N2 | 5 | On every valid CTRL (and every 5 ms): servo follows the wheel while NORMAL and CTRL fresh, else centers |
| drive | N3 | 5 | On every valid CTRL (and every 5 ms): brake / coast / PID target; dynamic brake unless NORMAL and CTRL fresh |
| pid | N3 | 6 | Every 10 ms: encoders -> Lab 2 feedforward + PI -> motor duty, while the mode is PID |
| blink | both | 5 | BLINK_F / BLINK_R: lamps follow the cockpit's BLINK as soon as it arrives; local hazards if the cockpit can't be heard or the zone is ISOLATED |
| test_btn | both | 5 | USER button B1 (ISR gives a semaphore, 50 ms debounce lockout): single press = local fault, double press = recover |
| hb_tx | both | 7 | HB every 20 ms (k_timer, no drift) on both buses |
| hb_mon | both | 8 | Every 5 ms: DEG_A/B, zone lost, then the zone state machine (zone.c) and the watchdog check |
| force | N2 | 9 | FORCE every 50 ms (dummy counter) |
| current | both | 10 | Current sensors every 20 ms (N2: servo, N3: both motors) |
| report | both | 12 | Console summary once a second |

**Shared state and blocking terms** (for the node RTA): the latest CTRL is
behind a spinlock (dispatch writes, steer/drive read, a few instructions).
On N3, `drive_lock` (a mutex, so priority inheritance applies) guards the
drive mode and every motor command; PID computes outside it and holds it only
to read the mode and write the duty, which bounds the blocking DRIVE sees
from PID. `motor.c` has its own mutex inside that.

## Lab 2 actuators and sensors

Ported from `lab2_backup/` with their Lab 2 pins (`docs/lab3_pin_map.md`):

| Lab 2 module | Now | Node | Changes |
|---|---|---|---|
| `drivers/servo.c`, `tasks/steering.c` | `servo.c`, `steer.c` | N2 | Driven by CTRL over CAN; wakes on each CTRL instead of polling every 20 ms; centers unless NORMAL and CTRL fresh |
| `drivers/motor.c` | `motor.c` | N3 | Pins from the devicetree; PA7 debug toggle is now PWM_SET |
| `drivers/encoder.c` | `encoder.c` | N3 | Unchanged |
| `tasks/motor_control.c` | `drive.c` | N3 | Split into DRIVE (on CTRL) and PID (10 ms instead of 20 ms); throttle/brake in CTRL counts: deadband 2, brake at >= 4; brake wins over throttle |
| `drivers/current_sensor.c`, `tasks/current_sensing.c` | `current_sensor.c` | both | Channels per node in `node_config.h`; reading only (no fault action yet) |
| `tasks/safety.c`, `tasks/blinkers.c`, `drivers/led.c` | - | - | Replaced by the Lab 3 error state machine and the Pi as blinker master (stage 4 / BLINK) |

Lab 2's command freshness limit carries over as `CTRL_TIMEOUT_MS` = 90 ms.

## Cockpit (N1)

`node1_pi/cockpit.c`, C with SocketCAN, chosen over Python for the Pi's share
of the 2 ms brake chain.

| Thread | Priority with `--rt P` | Job |
|---|---:|---|
| UDP_RX (main) | P | Wheel packet -> CTRL written to can0 then can1, nothing else first; then blinker and test-button logic |
| can_rx | P-1 | Both buses: heartbeats (presence per bus), FAULT_N2/N3, FORCE -> UDP to the proxy |
| timer | P-2 | HB_N1 every 20 ms; every 5 ms HB_MON, BLINK (master), queued FAULT_N1 |
| report | normal | Console summary once a second |

Writes are non-blocking: a bus with no ACK fills its queue and the write
fails instead of stalling the brake path. A degraded bus gets only
heartbeats, and as root the program bounces the interface when the bus goes
degraded so queued stale frames are dropped.

## Zone state and fault protocol

Implemented on all three nodes: `node1_pi/cockpit.c` and `zone.c` on the Nucleos.

- **Local fault**: something wrong in this zone. Cockpit: test button (wheel
  button 10), UDP_TIMEOUT (no wheel packet for 90 ms). Nucleos, highest first:
  TEST_BUTTON (B1), CTRL_RANGE / CTRL_MALFORMED (held 1 s, then cleared by the
  next valid CTRL), CTRL_TIMEOUT (no valid CTRL for 90 ms after boot). On
  entering one, the zone sends FAULT_Nx (code, detail) once; while it lasts
  every heartbeat carries LOCAL_FAULT, and byte 3 carries the most recent
  local fault code.
- **Zone state**:
  - BOOT until every other zone's heartbeat has been heard and (cockpit) wheel
    data / (Nucleos) a valid CTRL has arrived.
  - ISOLATED (Nucleos, after boot) when no other zone is heard on either bus:
    the fail-safe state - N3 brakes, N2 centers the servo, both flash hazards.
  - SYS_ERROR while the zone has a local fault, another zone is lost (66 ms on
    both buses), another zone reports a fault, or another zone's heartbeat says
    BOOT or ISOLATED (not ready).
  - NORMAL otherwise. Recovery is automatic, no reset. Another zone being in
    SYS_ERROR doesn't count, so zones can't hold each other in error.
- **A FAULT_Nx from another zone** marks it faulted at once (the fast path,
  checkoff 9's 100 ms). It is cleared by that zone's next heartbeat with
  LOCAL_FAULT = 0 that arrives more than 25 ms after the FAULT (an older
  heartbeat can overtake a FAULT, which has the higher priority).
- **PEER_LOST is informational**: each zone detects lost peers itself from
  heartbeats, so receiving PEER_LOST doesn't fault the sender.
- **Actuators** run only in NORMAL with fresh CTRL; in every other state N3
  dynamic-brakes and N2 centers. A test-button press or a rejected CTRL
  re-evaluates the state and wakes the actuator task immediately (R2.5 local
  fail within 10 ms).
- **Hazards**: the cockpit sends BLINK = hazard whenever it isn't NORMAL, and
  the STM32s follow it (front/rear in sync). An STM32 flashes hazards on its
  own when it's ISOLATED, can't hear the cockpit, or gets no BLINK for 1.1 s.
- **Watchdog**: each STM32's IWDG (250 ms) is fed every 100 ms only if HB_TX
  and the actuator task both checked in; a watchdog reset is reported at the
  next boot as WATCHDOG_RESET in heartbeat byte 3.

**Checkoff walk-through**

| Step | What happens |
|---|---|
| 1 cold start, proxy stopped | Cockpit BOOT + UDP_TIMEOUT, sends hazard BLINK; STM32s BOOT (no CTRL), ERR high everywhere. Proxy starts: cockpit boots, CTRL flows, STM32s boot, all NORMAL within ~40 ms |
| 5 one node off one bus | That node's DEG on within 49 ms; state unchanged everywhere |
| 7 the hop | N2 relays; N3 keeps CTRL, BLINK and HB_N1; all stay NORMAL |
| 8 one STM32 isolated | It goes ISOLATED (fail-safe, local hazards) after 66 ms; the other two SYS_ERROR within 71 ms. Restore: NORMAL again within ~50 ms, no reset |
| 9 test button on N2 | N2 SYS_ERROR at once, FAULT_N2; N3 brakes within ~5 ms; cockpit hazards within ~25 ms; ERR high on all three. Double press: NORMAL again |
| 10 bad input | Both STM32s reject the CTRL, send FAULT_N2 / FAULT_N3, stay in error 1 s, recover on the next valid CTRL |

## Status

Software complete for the checkoff sequence: CAN layer, heartbeats, HB_MON,
DEG LEDs, N2 relay, test points, CTRL checking, the zone state machine and
fault protocol on all three nodes, test buttons, blinkers (cockpit as master),
FORCE, the watchdog, the Lab 2 steering (N2) and drivetrain with PID (N3),
current sensors, and the cockpit program (N1). Not run on hardware yet.

Left for the report: WCET and latency measurements with the test points,
the response-time tables, and the fault-injection matrix with measured
recovery times. Current readings are reported but not acted on (no
MOTOR_OVERLOAD fault).
