# Lab 3 Part 3: real-time analysis (one bus)

Worst-case analysis of the template task set as implemented in `node2_front/`,
`node3_rear/` and `node1_pi/` (commit 9342b39), on **one bus with N2's relay
off**: each node sends one copy and receives one. Part 4 adds the second
bus and the relay.

**Status of the numbers.** No WCET has been measured yet. Every C below is
either a handout placeholder or an estimate from the SPI bytes the code moves,
and every Pi number except the Pi stage is assumed. All of them sit in one
place, the INPUTS block of `docs/analysis/lab3_rta.py`. Measure (see
"Measuring the inputs"), edit that block, and rerun:
`python3 docs/analysis/lab3_rta.py --pi-us <measured max>`. The script prints
the same tables as this document. The *structure* of the result (which terms
dominate, what fails as built and why) doesn't depend on the exact values.

## Result

| | As built | With the four changes below |
|---|---|---|
| Bus utilization, one bus | 11.3 % | 11.3 % (R4.6: 50 %) |
| CAN_RX (frame end -> handed to task), D = 0.3 ms | 0.75 / 0.98 ms (N2 / N3): **missed** | 0.22 ms |
| MCP2515 RX overrun (2 buffers: 3rd frame lost after 0.444 ms) | **possible** (0.62 / 0.85 ms) | no (0.19 ms) |
| DRIVE / BLINK local, D = 0.5 ms | **0.60 / 0.58 ms** | 0.35 / 0.35 ms |
| Brake chain UDP_RX -> PWM_OUT, D = 2 ms | Pi + 4.6 ms: **cannot be met** | Pi + 1.23 ms: **met if Pi stage <= 0.78 ms** |
| Steering, D = 50 ms | Pi + 24.3 ms | Pi + 21.1 ms |
| Blinker start, D = 100 ms | Pi + 26.0 ms | Pi + 25.0 ms |
| Blinker sync, D = 1 ms | **1.38 ms** | 0.34 ms |
| Test button: local fail / N3 brakes / hazards | 1.4 / 16-41 / 35-55 ms | 0.9 / 15-40 / 33-53 ms |
| Zone dead (66 ms silence): N3 brakes / hazards | 78 / 97 ms | 77 / 96 ms (3-4 ms margin) |

The sentence the report needs, with today's numbers: **the STM32 and bus side
of the brake chain takes 1.23 ms in the worst case once the four changes are
in, so R2.1/R2.2 hold only if the Pi's UDP_RX -> CMD_TX latency stays below
0.78 ms (0.88 ms if the chain ends at PWM_SET instead of the PWM pin). As
built, the chain can't be guaranteed even with an instant Pi**, for two
reasons that have nothing to do with the placeholder WCETs:

1. **The Pi queues CTRL behind its own frames.** Linux's `mcp251x` driver
   transmits from one TX buffer (TXB0) and the CAN netdev queue is FIFO. The
   cockpit's timer thread writes HB_N1, BLINK and FAULT_N1 on its own 5 ms /
   20 ms grid, so a wheel packet that lands just after one of those writes
   puts CTRL behind them. Each frame ahead costs its own arbitration and
   270 us on the wire, plus the Pi's TX-done -> reload turnaround. Worst
   case is 2.5 ms before CTRL even starts arbitrating. The Mac's and Pi's
   clocks drift, so the phase sweeps through this window and a minute of
   traffic will hit it. This never shows on UDP_RX -> CMD_TX, because CMD_TX
   toggles when `write()` returns, not when the frame goes on the wire.
2. **SPI priority inversion on the Nucleos.** The STM32 SPI driver runs in
   polling mode (no SPI thread). A transfer runs at the *caller's* priority,
   and SPI1's lock is a `k_sem` with no priority inheritance. When HB_TX (7),
   HB_MON (8) or FORCE_TX (9) is mid-transaction and a frame arrives, the
   MCP2515 driver thread blocks on that semaphore. Meanwhile every thread
   between it and the holder can run first: DISPATCH, DRIVE/STEER, BLINK,
   TEST_BTN, PID. On N3 that is a 0.41 ms block, which pushes the driver past
   the MCP2515's 0.444 ms overrun limit and stretches every receive path.

The SPI clock is also half what the overlay suggests. SPI1 runs from APB2 =
84 MHz and Zephyr picks the first 84 MHz / 2^n at or below
`spi-max-frequency`, so 4 MHz becomes **2.625 MHz**: 3 us per byte, and 52 us
of wire time to read one received frame.

## What has to change

| # | Change | Where | Effect |
|---|---|---|---|
| 1 | `k_sched_lock()` / `k_sched_unlock()` around `can_send()` in `queue_on()` | `canbus.c` (both Nucleos) | The SPI holder can't be preempted by another thread, so the driver waits at most one send (74 us at today's SPI clock, 51 us with change 2) instead of 0.26-0.41 ms. Every thread above the lowest sender pays that one send as blocking |
| 2 | `spi-max-frequency = <10000000>` on both MCP2515 nodes | `app.overlay` | Zephyr picks 84 / 16 = **5.25 MHz** (10.5 MHz would exceed the MCP2515's 10 MHz). RX job 89 -> 63 us. Check for errors on the breadboard wiring first; the earlier 1 MHz -> 4 MHz change already fixed lost frames |
| 3 | `PRIO_TEST_BTN` 5 -> 6 (with PID) | `lab3_config.h` | Deadline-monotonic: TEST_BTN has 10 ms, DRIVE/STEER/BLINK have 0.5-1 ms. Removes a 96-119 us same-priority job, which includes an SPI send, from DRIVE's and BLINK's response |
| 4 | The Pi writes HB_N1, BLINK and FAULT_N1 from the UDP thread right after CTRL. The timer thread only decides what is due; a fallback sends from the timer if no wheel packet arrives for 25 ms | `cockpit.c` | Nothing of N1's is ever queued ahead of CTRL: the CTRL bus stage drops from 3.03 to 0.69 ms. HB_N1 then follows the proxy's 20 ms, so check its jitter stays inside R4.5's ±10 %. Cheaper but weaker alternative: a `tc` prio qdisc with CTRL in band 0 still leaves one frame in TXB0 ahead of it (up to ~1.8 ms if it is a BLINK) |

Separate from the timing: **DIR_A (H-bridge IN1) never toggles in the current
code.** IN1 is 0 when driving forward (`set_inputs(0, 1, 1, 0)`), braking
`(0, 0, 0, 0)` and coasting. A scope capture "UDP_RX to DIR_A" (checkoff 4,
"Measuring the chains") will show nothing. End the brake chain at
**PWM_SET**, which `brake_locked()` / `drive_locked()` toggle on every motor
command, or at PWM_OUT, and say so in the report. Alternatively, move the
DIR_A test point to IN2 (PB13), which drops on a brake from forward drive.

## Model

- **Bus.** 500 kbit/s, tau = 2 us. Every catalog frame is DLC 8, so
  C = (47 + 64 + 24) x 2 = 270 us. The shortest frame (no stuffing) is
  111 bits = 222 us, so on one bus a node sees at most one frame-end event
  (received frame or own TX done) every 222 us. Davis et al. (2007) sufficient
  test with B = B_MAX = 270 us for every frame, including FORCE (its own
  previous instance can push through). FAULT is one ID per sender (0x020-0x022);
  all three are counted at their 20 ms minimum interarrival.
- **STM32 threads** (`lab3_config.h`; lower = higher priority). The MCP2515
  driver threads are created with `K_PRIO_COOP(2)`: **cooperative**, above
  every application thread and never preempted by one. The driver handles
  every event non-preemptively and FIFO, and receives *every* frame on the bus
  (RXM = 11, filtering in software), including FORCE at N3. It also takes an
  interrupt for each of the node's own completed transmissions.
- **Driver and SPI costs** (`can_mcp2515.c`). Each SPI transaction is
  a + n x b, with a = 8 us (lock, configure, CS, unlock), assumed, and
  b = wire time + 0.5 us polling gap, assumed. Costs per event:
  - **RX job:** ISR, READ CANINTF (3 B), READ RX BUFFER (14 B), callback,
    INT pin check. **89 us** at 2.625 MHz.
  - **TX-done job:** READ CANINTF plus BIT MODIFY (4 B). **52 us**.
  - **Send** (runs in the caller): LOAD TX BUFFER (14 B) plus RTS (1 B).
    **74 us**.
  - With change 2 (5.25 MHz) the RX job is 63 us, and driver + DISPATCH is
    78 us, close to the handout's 80 us CAN_RX placeholder.
- **Interference.** Equal-priority threads are treated as higher (FIFO,
  conservative). Driver and DISPATCH jobs are bus-paced: one per 222 us.
  DRIVE, STEER and BLINK also wake on their 5 / 5 / 10 ms poll timeouts,
  and those jobs interfere too.
- **Blocking.**
  - Every thread: 10 us for the longest IRQ-off spinlock (`zone_update`).
  - DRIVE: 25 us for PID's longest `drive_lock` section. It is a k_mutex, so
    priority inheritance applies.
  - TEST_BTN: `update_lock`, held by HB_MON across its FAULT send.
  - The driver: the SPI semaphore, explained above.
- **Paths.** A receive path (driver -> DISPATCH -> actuator) is bounded two
  ways: by the sum of the stage response times (the handout's method), and as
  one job at the lowest priority on the path (every stage is at or above it,
  so the path ends when that busy window ends). Both are valid upper bounds;
  the tables report the smaller.
- **Actuator outputs.** Timer compare preload is on in Zephyr's STM32 PWM
  driver. A new duty reaches PWM_OUT within one 100 us period. A new servo
  pulse reaches SRV within one **20 ms** period, which dominates the steering
  chain.
- **Pi.** The Pi stage (UDP_RX -> CMD_TX) is measured and left as "Pi"
  until then. Two Pi numbers are assumed: write()/TX-done -> frame loaded in
  TXB0 = 150 us, and timer-thread wake-up latency = 1.5 ms.

## Tables

Generated by `python3 docs/analysis/lab3_rta.py`. All times in us. "Pi" is the measured Pi stage. C sources: "handout placeholder" and "estimate" are assumed, not measured.

### As built (commit 9342b39)

SPI 2.625 MHz (3.05 us/byte on the wire; one received frame = 17 bytes = 52 us of wire time). Derived: driver RX job 89 us, TX-done job 52 us, send on one bus 74 us, longest SPI transaction 58 us.

#### 3.1 Node response times

##### N2

| Task | Prio | T (us) | C (us) | B (us) | R (us) | D (us) | Met | C: source / note |
|---|---:|---:|---:|---:|---:|---:|:-:|---|
| MCP2515 driver (RX / TX done) | coop | 222 (bus-paced) | 89 | 264 | 620 | - |  | estimate from code (SPI bytes / calls); RX 89 / TX-done 52; SPI 2.62 MHz; <= 1 event per 222 us |
| DISPATCH | 4 | 222 (bus-paced) | 15 | 10 | 129 | - |  | estimate from code (SPI bytes / calls); dedup, CTRL CRC/range, route |
| STEER | 5 | 20,000 | 50 | 10 | 581 | 1,000 | yes | handout placeholder; on CTRL |
| BLINK_F | 5 | 20,000 | 20 | 10 | 581 | 500 | **NO** | handout placeholder; on BLINK |
| TEST_BTN | 5 | 50,000 | 119 | 157 | 832 | 10,000 | yes | estimate from code (SPI bytes / calls); 20 placeholder + zone_update 25 + FAULT send 74 |
| HB_TX | 7 | 20,000 | 85 | 58 | 818 | 20,000 | yes | estimate from code (SPI bytes / calls); one bus (template 120 = two buses) |
| HB_MON | 8 | 5,000 | 129 | 58 | 1,051 | 5,000 | yes | estimate from code (SPI bytes / calls); 30 placeholder + zone_update 25 + FAULT send 74; every 5 ms |
| FORCE_TX | 9 | 50,000 | 80 | 10 | 1,083 | 50,000 | yes | estimate from code (SPI bytes / calls) |
| CURRENT | 10 | 20,000 | 300 | 10 | 1,695 | 20,000 | yes | estimate from code (SPI bytes / calls); 16 blocking adc_read calls, CPU part only |
| REPORT | 12 | 1,000,000 | 90,000 | 10 | 193,321 | 1,000,000 | yes | estimate from code (SPI bytes / calls); ~1000 chars/s of printk, polled UART at 115200 |
| **CAN_RX** = driver + DISPATCH, frame EOF -> handed to task | | | | | 749 | 300 | **NO** | min(stage sum 749, one busy window 784) |
| **RX overrun check**: driver done before the 3rd frame ends | | | | | 620 | 444 | **NO** | MCP2515 has 2 RX buffers |

CPU utilization at actual rates: 19.6 % (REPORT's polled printk is 9 % of it)

##### N3

| Task | Prio | T (us) | C (us) | B (us) | R (us) | D (us) | Met | C: source / note |
|---|---:|---:|---:|---:|---:|---:|:-:|---|
| MCP2515 driver (RX / TX done) | coop | 222 (bus-paced) | 89 | 409 | 854 | - |  | estimate from code (SPI bytes / calls); RX 89 / TX-done 52; SPI 2.62 MHz; <= 1 event per 222 us |
| DISPATCH | 4 | 222 (bus-paced) | 15 | 10 | 129 | - |  | estimate from code (SPI bytes / calls); dedup, CTRL CRC/range, route |
| DRIVE | 5 | 20,000 | 50 | 25 | 596 | 500 | **NO** | handout placeholder; on CTRL |
| BLINK_R | 5 | 20,000 | 20 | 10 | 581 | 500 | **NO** | handout placeholder; on BLINK |
| TEST_BTN | 5 | 50,000 | 119 | 357 | 1,240 | 10,000 | yes | estimate from code (SPI bytes / calls); 20 placeholder + zone_update 25 + FAULT send 74 |
| PID | 6 | 10,000 | 200 | 10 | 885 | 10,000 | yes | handout placeholder |
| HB_TX | 7 | 20,000 | 85 | 58 | 1,226 | 20,000 | yes | estimate from code (SPI bytes / calls); one bus (template 120 = two buses) |
| HB_MON | 8 | 5,000 | 129 | 10 | 1,307 | 5,000 | yes | estimate from code (SPI bytes / calls); 30 placeholder + zone_update 25 + FAULT send 74; every 5 ms |
| CURRENT | 10 | 20,000 | 500 | 10 | 2,327 | 20,000 | yes | estimate from code (SPI bytes / calls); 32 blocking adc_read calls, CPU part only |
| REPORT | 12 | 1,000,000 | 90,000 | 10 | 206,876 | 1,000,000 | yes | estimate from code (SPI bytes / calls); ~1000 chars/s of printk, polled UART at 115200 |
| **CAN_RX** = driver + DISPATCH, frame EOF -> handed to task | | | | | 983 | 300 | **NO** | min(stage sum 983, one busy window 1,033) |
| **RX overrun check**: driver done before the 3rd frame ends | | | | | 854 | 444 | **NO** | MCP2515 has 2 RX buffers |

CPU utilization at actual rates: 22.5 % (REPORT's polled printk is 9 % of it)

#### 3.2 Message response times (one bus)

| ID | Message | From | T | J | C | B | w (handout) | R (handout) | own-queue term | w (as sent) | R (as sent) | D | Met |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|:-:|
| 0x010 | CTRL | N1 | 20,000 | Pi | 270 | 270 | 270 | Pi + 540 | 1,410 | 2,760 | Pi + 3,030 | 2,000 | see 3.3 |
| 0x020 | FAULT_N1 | N1 | 20,000 | 1,500 | 270 | 270 | 540 | 2,310 | 1,410 | 2,760 | 4,530 | 100,000 | yes |
| 0x021 | FAULT_N2 | N2 | 20,000 | 1,051 | 270 | 270 | 810 | 2,131 | 540 | 2,430 | 3,751 | 100,000 | yes |
| 0x022 | FAULT_N3 | N3 | 20,000 | 1,307 | 270 | 270 | 1,080 | 2,657 | 270 | 1,890 | 3,467 | 100,000 | yes |
| 0x100 | HB_N1 | N1 | 20,000 | 1,500 | 270 | 270 | 1,350 | 3,120 | 1,410 | 2,760 | 4,530 | 20,000 | yes |
| 0x101 | HB_N2 | N2 | 20,000 | 818 | 270 | 270 | 1,620 | 2,708 | 270 | 2,430 | 3,518 | 20,000 | yes |
| 0x102 | HB_N3 | N3 | 20,000 | 1,226 | 270 | 270 | 1,890 | 3,386 | 0 | 1,890 | 3,386 | 20,000 | yes |
| 0x200 | BLINK | N1 | 20,000 | 1,500 | 270 | 270 | 2,160 | 3,930 | 1,410 | 2,760 | 4,530 | 100,000 | yes |
| 0x300 | FORCE | N2 | 50,000 | 1,083 | 270 | 270 | 2,430 | 3,783 | 0 | 2,430 | 3,783 | 50,000 | yes |

Bus utilization, one bus: 11.34 % (R4.6 ceiling 50 %). All times in us.

#### 3.3 End-to-end chains

**Brake / throttle: UDP_RX (N1) -> PWM_OUT (N3)** (deadline 2,000 us)

| Stage | us | From |
|---|---:|---|
| Pi stage, UDP_RX -> CMD_TX (measured max) | Pi | scope, 3.3 |
| CTRL after write(): Pi driver load, Pi queue, bus (own-queue + w + C) | 3,030 | 3.2 R as sent - J |
| N3: MCP2515 driver + DISPATCH + DRIVE (to PWM_SET) | 1,500 | 3.1 N3, min(sum 1,579, busy window 1,500) |
| PWM compare preload -> PWM_OUT pin | 100 | 10 kHz |
| **Total** | **Pi + 4,630** | met if the Pi stage is <= **-2,630** |

**Steering: UDP_RX (N1) -> SRV (N2)** (deadline 50,000 us)

| Stage | us | From |
|---|---:|---|
| Pi stage, UDP_RX -> CMD_TX (measured max) | Pi | scope, 3.3 |
| CTRL after write(): Pi driver load, Pi queue, bus | 3,030 | 3.2 |
| N2: driver + DISPATCH + STEER | 1,251 | 3.1 N2, min(sum 1,330, busy window 1,251) |
| Servo compare preload -> new pulse width on SRV | 20,000 | 50 Hz |
| **Total** | **Pi + 24,281** | met if the Pi stage is <= **25,719** |

**Blinker start: UDP_RX (N1) -> FL (N2) / RL (N3)** (deadline 100,000 us)

| Stage | us | From |
|---|---:|---|
| Pi stage, UDP_RX -> CMD_TX (measured max) | Pi | scope, 3.3 |
| Cockpit timer thread: next 5 ms tick, or 20 ms after the previous BLINK | 20,000 | cockpit.c |
| Pi timer-thread wake-up latency | 1,500 | assumed |
| BLINK after write(): Pi driver load, Pi queue, bus | 3,030 | 3.2 |
| Slower node: driver + DISPATCH + BLINK task -> lamp GPIO | 1,500 | 3.1 |
| **Total** | **Pi + 26,030** | met if the Pi stage is <= **73,970** |

**Blinker sync** (R2.4: FL and RL edges within 1,000 us). One BLINK frame ends at the same instant at N2 and N3 (same bus). Skew <= max(WCRT_N2 - BCRT_N3, WCRT_N3 - BCRT_N2) = max(1,251 - 124, 1,500 - 124) = **1,376 us** -> **NO**. (BCRT = the path's C with no interference.)

**Error propagation, test button on N2** (R2.5: local fail 10 ms, system error 100 ms)

| Path | us | D | Met |
|---|---:|---:|:-:|
| Local fail on N2: TEST_BTN (zone_update sets ERR) + STEER wakes and centers | 1,413 | 10,000 | yes |
| N3 brakes, via FAULT_N2: TEST_BTN + bus + N3 RX + next HB_MON tick + next DRIVE poll | 16,418 | 100,000 | yes |
|   same, FAULT_N2 held by the 20 ms FAULT spacing | 41,418 | 100,000 | yes |
| N3 brakes, via the next HB_N2 carrying LOCAL_FAULT (FAULT frame lost) | 36,418 | 100,000 | yes |
| Hazards on FL/RL, via FAULT_N2 -> cockpit monitor -> hazard BLINK | 34,562 | 100,000 | yes |
| Hazards on FL/RL, via HB_N2 (FAULT frame lost) | 54,562 | 100,000 | yes |
| Zone dead (handout chain, checkoff 8): N3 brakes after 66 ms silence | 77,903 | 100,000 | yes |
| Zone dead: hazards on FL/RL via the cockpit | 97,030 | 100,000 | yes |


### With changes 1-4

SPI 5.250 MHz (1.52 us/byte on the wire; one received frame = 17 bytes = 26 us of wire time). Derived: driver RX job 63 us, TX-done job 41 us, send on one bus 51 us, longest SPI transaction 36 us.

#### 3.1 Node response times

##### N2

| Task | Prio | T (us) | C (us) | B (us) | R (us) | D (us) | Met | C: source / note |
|---|---:|---:|---:|---:|---:|---:|:-:|---|
| MCP2515 driver (RX / TX done) | coop | 222 (bus-paced) | 63 | 61 | 187 | - |  | estimate from code (SPI bytes / calls); RX 63 / TX-done 41; SPI 5.25 MHz; <= 1 event per 222 us |
| DISPATCH | 4 | 222 (bus-paced) | 15 | 51 | 144 | - |  | estimate from code (SPI bytes / calls); dedup, CTRL CRC/range, route |
| STEER | 5 | 20,000 | 50 | 51 | 347 | 1,000 | yes | handout placeholder; on CTRL |
| BLINK_F | 5 | 20,000 | 20 | 51 | 347 | 500 | yes | handout placeholder; on BLINK |
| TEST_BTN | 6 | 50,000 | 96 | 127 | 597 | 10,000 | yes | estimate from code (SPI bytes / calls); 20 placeholder + zone_update 25 + FAULT send 51 |
| HB_TX | 7 | 20,000 | 62 | 51 | 583 | 20,000 | yes | estimate from code (SPI bytes / calls); one bus (template 120 = two buses) |
| HB_MON | 8 | 5,000 | 106 | 51 | 767 | 5,000 | yes | estimate from code (SPI bytes / calls); 30 placeholder + zone_update 25 + FAULT send 51; every 5 ms |
| FORCE_TX | 9 | 50,000 | 57 | 10 | 783 | 50,000 | yes | estimate from code (SPI bytes / calls) |
| CURRENT | 10 | 20,000 | 300 | 10 | 1,239 | 20,000 | yes | estimate from code (SPI bytes / calls); 16 blocking adc_read calls, CPU part only |
| REPORT | 12 | 1,000,000 | 90,000 | 10 | 153,054 | 1,000,000 | yes | estimate from code (SPI bytes / calls); ~1000 chars/s of printk, polled UART at 115200 |
| **CAN_RX** = driver + DISPATCH, frame EOF -> handed to task | | | | | 217 | 300 | yes | min(stage sum 331, one busy window 217) |
| **RX overrun check**: driver done before the 3rd frame ends | | | | | 187 | 444 | yes | MCP2515 has 2 RX buffers |

CPU utilization at actual rates: 17.9 % (REPORT's polled printk is 9 % of it)

##### N3

| Task | Prio | T (us) | C (us) | B (us) | R (us) | D (us) | Met | C: source / note |
|---|---:|---:|---:|---:|---:|---:|:-:|---|
| MCP2515 driver (RX / TX done) | coop | 222 (bus-paced) | 63 | 61 | 187 | - |  | estimate from code (SPI bytes / calls); RX 63 / TX-done 41; SPI 5.25 MHz; <= 1 event per 222 us |
| DISPATCH | 4 | 222 (bus-paced) | 15 | 51 | 144 | - |  | estimate from code (SPI bytes / calls); dedup, CTRL CRC/range, route |
| DRIVE | 5 | 20,000 | 50 | 51 | 347 | 500 | yes | handout placeholder; on CTRL |
| BLINK_R | 5 | 20,000 | 20 | 51 | 347 | 500 | yes | handout placeholder; on BLINK |
| TEST_BTN | 6 | 50,000 | 96 | 127 | 875 | 10,000 | yes | estimate from code (SPI bytes / calls); 20 placeholder + zone_update 25 + FAULT send 51 |
| PID | 6 | 10,000 | 200 | 51 | 799 | 10,000 | yes | handout placeholder |
| HB_TX | 7 | 20,000 | 62 | 51 | 861 | 20,000 | yes | estimate from code (SPI bytes / calls); one bus (template 120 = two buses) |
| HB_MON | 8 | 5,000 | 106 | 10 | 1,004 | 5,000 | yes | estimate from code (SPI bytes / calls); 30 placeholder + zone_update 25 + FAULT send 51; every 5 ms |
| CURRENT | 10 | 20,000 | 500 | 10 | 1,738 | 20,000 | yes | estimate from code (SPI bytes / calls); 32 blocking adc_read calls, CPU part only |
| REPORT | 12 | 1,000,000 | 90,000 | 10 | 161,832 | 1,000,000 | yes | estimate from code (SPI bytes / calls); ~1000 chars/s of printk, polled UART at 115200 |
| **CAN_RX** = driver + DISPATCH, frame EOF -> handed to task | | | | | 217 | 300 | yes | min(stage sum 331, one busy window 217) |
| **RX overrun check**: driver done before the 3rd frame ends | | | | | 187 | 444 | yes | MCP2515 has 2 RX buffers |

CPU utilization at actual rates: 20.8 % (REPORT's polled printk is 9 % of it)

#### 3.2 Message response times (one bus)

| ID | Message | From | T | J | C | B | w (handout) | R (handout) | own-queue term | w (as sent) | R (as sent) | D | Met |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|:-:|
| 0x010 | CTRL | N1 | 20,000 | Pi | 270 | 270 | 270 | Pi + 540 | 150 | 420 | Pi + 690 | 2,000 | see 3.3 |
| 0x020 | FAULT_N1 | N1 | 20,000 | 1,500 | 270 | 270 | 540 | 2,310 | 570 | 840 | 2,610 | 100,000 | yes |
| 0x021 | FAULT_N2 | N2 | 20,000 | 767 | 270 | 270 | 810 | 1,847 | 540 | 2,430 | 3,467 | 100,000 | yes |
| 0x022 | FAULT_N3 | N3 | 20,000 | 1,004 | 270 | 270 | 1,080 | 2,354 | 270 | 1,890 | 3,164 | 100,000 | yes |
| 0x100 | HB_N1 | N1 | 20,000 | 1,500 | 270 | 270 | 1,350 | 3,120 | 990 | 1,800 | 3,570 | 20,000 | yes |
| 0x101 | HB_N2 | N2 | 20,000 | 583 | 270 | 270 | 1,620 | 2,473 | 270 | 2,430 | 3,283 | 20,000 | yes |
| 0x102 | HB_N3 | N3 | 20,000 | 861 | 270 | 270 | 1,890 | 3,021 | 0 | 1,890 | 3,021 | 20,000 | yes |
| 0x200 | BLINK | N1 | 20,000 | 1,500 | 270 | 270 | 2,160 | 3,930 | 1,410 | 2,760 | 4,530 | 100,000 | yes |
| 0x300 | FORCE | N2 | 50,000 | 783 | 270 | 270 | 2,430 | 3,483 | 0 | 2,430 | 3,483 | 50,000 | yes |

Bus utilization, one bus: 11.34 % (R4.6 ceiling 50 %). All times in us.

#### 3.3 End-to-end chains

**Brake / throttle: UDP_RX (N1) -> PWM_OUT (N3)** (deadline 2,000 us)

| Stage | us | From |
|---|---:|---|
| Pi stage, UDP_RX -> CMD_TX (measured max) | Pi | scope, 3.3 |
| CTRL after write(): Pi driver load, Pi queue, bus (own-queue + w + C) | 690 | 3.2 R as sent - J |
| N3: MCP2515 driver + DISPATCH + DRIVE (to PWM_SET) | 435 | 3.1 N3, min(sum 678, busy window 435) |
| PWM compare preload -> PWM_OUT pin | 100 | 10 kHz |
| **Total** | **Pi + 1,225** | met if the Pi stage is <= **775** |

**Steering: UDP_RX (N1) -> SRV (N2)** (deadline 50,000 us)

| Stage | us | From |
|---|---:|---|
| Pi stage, UDP_RX -> CMD_TX (measured max) | Pi | scope, 3.3 |
| CTRL after write(): Pi driver load, Pi queue, bus | 690 | 3.2 |
| N2: driver + DISPATCH + STEER | 435 | 3.1 N2, min(sum 678, busy window 435) |
| Servo compare preload -> new pulse width on SRV | 20,000 | 50 Hz |
| **Total** | **Pi + 21,125** | met if the Pi stage is <= **28,875** |

**Blinker start: UDP_RX (N1) -> FL (N2) / RL (N3)** (deadline 100,000 us)

| Stage | us | From |
|---|---:|---|
| Pi stage, UDP_RX -> CMD_TX (measured max) | Pi | scope, 3.3 |
| Cockpit timer thread: next 5 ms tick, or 20 ms after the previous BLINK | 20,000 | cockpit.c |
| Pi timer-thread wake-up latency | 1,500 | assumed |
| BLINK after write(): Pi driver load, Pi queue, bus | 3,030 | 3.2 |
| Slower node: driver + DISPATCH + BLINK task -> lamp GPIO | 435 | 3.1 |
| **Total** | **Pi + 24,965** | met if the Pi stage is <= **75,035** |

**Blinker sync** (R2.4: FL and RL edges within 1,000 us). One BLINK frame ends at the same instant at N2 and N3 (same bus). Skew <= max(WCRT_N2 - BCRT_N3, WCRT_N3 - BCRT_N2) = max(435 - 98, 435 - 98) = **337 us** -> yes. (BCRT = the path's C with no interference.)

**Error propagation, test button on N2** (R2.5: local fail 10 ms, system error 100 ms)

| Path | us | D | Met |
|---|---:|---:|:-:|
| Local fail on N2: TEST_BTN (zone_update sets ERR) + STEER wakes and centers | 944 | 10,000 | yes |
| N3 brakes, via FAULT_N2: TEST_BTN + bus + N3 RX + next HB_MON tick + next DRIVE poll | 14,865 | 100,000 | yes |
|   same, FAULT_N2 held by the 20 ms FAULT spacing | 39,865 | 100,000 | yes |
| N3 brakes, via the next HB_N2 carrying LOCAL_FAULT (FAULT frame lost) | 34,865 | 100,000 | yes |
| Hazards on FL/RL, via FAULT_N2 -> cockpit monitor -> hazard BLINK | 33,262 | 100,000 | yes |
| Hazards on FL/RL, via HB_N2 (FAULT frame lost) | 53,262 | 100,000 | yes |
| Zone dead (handout chain, checkoff 8): N3 brakes after 66 ms silence | 77,351 | 100,000 | yes |
| Zone dead: hazards on FL/RL via the cockpit | 95,965 | 100,000 | yes |

## Measuring the Pi stage

The handout wants UDP_RX -> CMD_TX over at least a minute, as max and 99th
percentile, with and without load. Run it in the configuration you will use at
checkoff:

```
sudo ./cockpit --rt 80 --cpu 3                  # SCHED_FIFO, pinned, mlockall
# load, on the other three cores:
taskset -c 0-2 stress-ng --cpu 3 --io 2 --vm 1 --vm-bytes 128M --timeout 120s
```

- Trigger on UDP_RX (row 3), measure to the next CMD_TX edge (row 4). Use
  infinite persistence or the scope's histogram/measurement statistics for the
  max; a logic-analyzer export gives the p99. The cockpit's own console line
  (`UDP -> CAN write latency: mean / max`) is a software cross-check, not the
  number to report.
- Do four runs: {no load, stress-ng} x {`--rt`, no `--rt`}. Put the **max
  with stress-ng and `--rt`** in the chain. It is the configuration being
  claimed, and a deadline needs a worst case, not a p99. Even that max is only
  an observation, not a bound (Linux gives none), so say so in the report.
- In the same session, trigger on UDP_RX and probe **N3's CAN_RX_A** with
  persistence on. That shows the Pi stage, the Pi's queue and the bus
  together. As built, the HB_N1-ahead-of-CTRL tail (finding 1) shows up here
  as a second cluster about 0.4-0.7 ms later than the rest, but not on CMD_TX.
  With change 4 it should disappear. That capture is the evidence for the
  change.
- Also measure CMD_TX -> start of frame on A_H with the bus idle. That is the
  assumed 150 us `DELTA_PI` (mcp251x workqueue + SPI load).

## Measuring the inputs (replace the assumptions)

A GPIO toggle around a thread body measures *response* time, including
preemption. To get C, measure with nothing else able to run: stop the cockpit
and inject single frames with `cansend` from the Pi, so the node sees one frame
at a time. Take the max over a few hundred samples. Cross-check with Zephyr's
per-thread cycle counts (`CONFIG_SCHED_THREAD_USAGE`,
`k_thread_runtime_stats_get()`), which exclude preemption but give totals, not
maxima.

| Input (INPUTS block) | Probe | Notes |
|---|---|---|
| `SPI_A`, `SPI_GAP` | Module CS (PB6 for bus A): width of each low pulse. The 3-byte CANINTF read gives a + 3b; READ RX BUFFER gives a + 14b | Also confirms SCK = 2.625 MHz |
| Driver RX job | Module INT (D2/PA10, bus A) falling edge -> CAN_RX_A toggle | ISR + coop thread wake + both SPI reads, up to rx_cb |
| DISPATCH + DRIVE | CAN_RX_A -> PWM_SET, brake pressed, single CTRL via `cansend` | Split by adding a temporary toggle in `ctrl_on_frame()` |
| DISPATCH + BLINK task | CAN_RX_A -> FL (N2) / RL (N3) | |
| HB_TX send | CAN_TX_A edge -> end of the RTS CS pulse | `C_SEND_1BUS` |
| STEER, PID, HB_MON, CURRENT, REPORT | No test point. Add a temporary toggle, or keep the placeholder and label it | |

## Answers to the guiding questions

**3.1, SPI as a shared resource.** If HB_TX is loading a TX buffer when a frame
lands, the driver thread waits for the SPI semaphore. That wait is
**blocking** (a lower thread holds a resource). It isn't bounded by the one
transaction, because the semaphore has no priority inheritance: every thread
between the driver and HB_TX can run first. **No thread runs SPI**: in
polling mode a transfer runs at its caller's priority. Receive reads run at
the driver's cooperative priority, transmits at the sending thread's.
Change 1 turns this into plain blocking of one send.

**3.1, driver RX priority.** `CONFIG_CAN_MCP2515_INT_THREAD_PRIO=2` ->
`K_PRIO_COOP(2)` = -14: cooperative, above DRIVE and everything else. If it
were below DRIVE, received frames would wait in the MCP2515's two RX
buffers while application threads run. The third frame would be lost after
0.444 ms, and every receive path would inherit the application load. Above
(the current setup), every frame on the bus, wanted or not, plus every own TX
completion, costs DRIVE one non-preemptible driver job (89 us). That is the
largest interference term in every receive row.

**3.1, WCET from a GPIO toggle.** See "Measuring the inputs": isolate the
stimulus so nothing can preempt, take the max, and cross-check with
per-thread cycle counts.

**3.2, three TX buffers.** The MCP2515 picks among its own pending buffers by
the TXP bits, then by **highest buffer number**, not by ID. Zephyr's driver
fills the lowest free buffer and never sets TXP, so N2's FAULT_N2 can wait
behind HB_N2 and FORCE, and N3's FAULT_N3 behind HB_N3. The "own-queue
term" column adds them: hp(m) grows to include the node's own lower-priority
frames, plus anything from other nodes that can beat those. The Pi is worse:
`mcp251x` uses TXB0 only, so its frames go out in `write()` order (finding 1).

**3.2, after arbitration.** R_m ends at the end of frame, when the frame is
complete in the receiver's RX buffer. The INT line, EXTI ISR, coop-thread
wake-up and SPI read all belong to the receiving node's driver row in 3.1,
and the chains add them separately.

**3.2, sporadic messages.**
- **FAULT:** at least 20 ms apart per sender, enforced by `FAULT_MIN_GAP_MS`
  in `zone.c` (a queued FAULT waits for the next HB_MON tick after the gap)
  and `MIN_EVENT_GAP_NS` in `cockpit.c`.
- **BLINK:** at least 20 ms apart, enforced by `MIN_EVENT_GAP_NS` for changes,
  plus a 500 ms toggle.

Both are analysed at T = 20 ms. FAULT is one ID per sender, so three FAULT
frames can share a 20 ms window, and the bus table counts all three.

**3.3, the 2 ms budget.** We don't control:
- Linux's socket wake-up and scheduling (the Pi stage)
- the `mcp251x` one-buffer FIFO
- bus arbitration against N2's and N3's frames (bounded by 3.2)

What we did about them:
- **Pi stage:** SCHED_FIFO + core pinning + `mlockall` (`--rt`, `--cpu`).
  CTRL is written before any other work in the UDP thread, and writes are
  non-blocking.
- **Pi FIFO:** change 4.
- **STM32 side:** the driver above every application thread, DISPATCH next,
  and changes 1-3.

**3.3, three missed heartbeats.** The zone-failed threshold is
3 x 22 = 66 ms (each heartbeat may be 10 % late). HB_MON runs every
**5 ms**, not 20 ms, so detection is <= 71 ms after the last heartbeat
(with a 20 ms monitor it would be 86 ms). The chain fits:
- N3 brakes within 77-78 ms.
- Hazards show within 96-97 ms, only 3-4 ms inside 100 ms. The cockpit holds
  a hazard BLINK for up to 20 ms when it sent any BLINK in the last 20 ms.
  Letting a change to hazard bypass that spacing (count BLINK at 5 ms for one
  instance in the bus table, still under 12 % utilization) would buy 15 ms.
- In practice the test button doesn't take this path. N2 keeps sending
  heartbeats (with LOCAL_FAULT) and sends FAULT_N2 at once, so N3 brakes in
  15-40 ms.

**3.3, blinker sync.** Clocks don't matter: both lamps change on the same
frame, which ends at the same instant at both nodes. What differs is each
node's receive path:
- blocking (SPI inversion, as built)
- other frames' driver jobs
- same-priority jobs: DRIVE on N3, STEER on N2, own poll

The bound max(WCRT_N2 - BCRT_N3, WCRT_N3 - BCRT_N2) is not symmetric in
general. N3 carries DRIVE and PID, N2 carries STEER and FORCE_TX. With the
changes the two WCRTs happen to coincide (PID and FORCE_TX are below BLINK,
and DRIVE and STEER have the same C). On two buses (Part 4) the copies end at
different times on A and B, and that offset adds to the skew.
