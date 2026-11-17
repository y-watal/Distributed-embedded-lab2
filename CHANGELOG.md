# Changelog

Lab 5 (Raft consensus and time synchronization) changes on the `lab5` branch,
newest first. Each entry names its commit, what changed, why, and what the
bus showed afterwards. Wire formats and rules are in
[docs/lab5_protocol.md](docs/lab5_protocol.md); the plan is in
[roadmap.md](roadmap.md).

## 2026-11-14 (uncommitted, not yet flashed)

### Relayed Raft fragments retry for a TX buffer
- **Changed:** N2's relay sends a bulk Raft fragment with
  `canbus_forward_try()` in 250 µs slices for up to 2 ms
  (`RELAY_RAFT_TX_WAIT_US`), and forwards waiting CTRL and FAULT frames
  between tries. A fragment it still cannot send is counted as `raftlost` on
  the relay report line. The relay queue grows from 16 to 32 entries.
- **Why:** in the 17:48 hop capture, N2 failed to relay 13–42 frames/s on bus
  B (3–13 on A) because relayed frames used `K_NO_WAIT`. N3 never received the
  6-fragment APPEND for entry 20, so its log stayed at commit 19 for 28 s.

### Client waits for its own commit before re-reporting a link
- **Changed:** `CLIENT_REPLY COMMITTED idx=N` records N for that subject. A
  node's own link that its graph disagrees with is not reported again until N
  is applied locally, or until `RAFT_CLIENT_REPAIR_HOLD_MS` (1 s) passes.
- **Why:** N3 re-sent `CANLINK_DOWN N3 A` about 10 times a second while it
  waited for entry 20, which added load to the relay.
- **Tests:** `test_committed_reply_holds_graph_repair` in `raft_agent_test.c`.

### Clock synchronization (Part 6)
- **Added:** `shared/timesync.c` (offset slewing, the drift ratio, holdover),
  STM32 `gtime.c`, and Pi `gtime.c`. REF is stamped per bus with global time,
  N2 restamps relayed REF, and a PPS thread prints one `SYNC:` line per global
  second. Details are in docs/lab5_protocol.md, "Time synchronization from
  REF".
- **Tests:** `tests/timesync_test.c` uses a +60 ppm follower with ±30 µs
  receive jitter. The model stays within 27 µs and measures −63 ppm. A
  300 µs handover is slewed 50 µs per REF.
- **Hop capture 20:42-20:47 (N1 off B, then N3 off A, both restored):** N2
  and N3 committed entries 13-16, one per link change, within the same 1 s
  report, with matching log hashes. `raftlost` stayed at 0, and both nodes
  stayed synced the whole time, N3 through the relay as well. REF error had a
  median of 12-14 us and a 95th percentile of about 95 us. Drift was +51 ppm
  on N2 and +60 ppm on N3. N2 still lost 5-16 relayed non-Raft frames a
  second on bus B, so N3 got only 44-48 CTRL a second.

### Lab 5 test-point pins
- PPS, SYNC, LEADER and REF_RX on PC10, PC11, PC12 and PD2 (morpho CN7 pins
  1-4) on both STM32s, since no Arduino pin is free. On the Pi they are
  GPIO12, 21, 17 and 27 (header pins 32, 40, 11 and 13). See
  docs/lab3_pin_map.md, "Lab 5 pins".
- **Pending:** blinkers driven from global time, and calibrating
  `TIMESYNC_REF_DELAY_US` against the scope.

### A returning node is no longer silenced by its own NODE_DOWN
- **Seen:** after the Pi was powered off and came back with the proxy
  running, hazards stayed at 2 Hz. On the bus the Pi sent only heartbeats:
  no CTRL, no BLINK, no APPEND_ACK, no CLIENT_UPDATE. N3 was leading; N2 and
  N3 reported CTRL timeouts and fell back to stale-BLINK hazards.
- **Why:** while the Pi was down the log committed `NODE_DOWN N1`, which
  also marks N1's links down. The returning Pi replayed it, and
  `raft_route_send_on_bus()` refused both buses because the graph said its
  own links were down. Only heartbeats bypass routing. The reports that
  would fix the graph (`NODE_UP N1`, `CANLINK_UP N1`) were blocked by the
  same rule, so it never recovered. A rebooted STM32 would have done the same.
- **Changed:** a node's own link state comes from its local DEG detection,
  checked before routing. The graph only decides whether a bus has anyone
  to hear the frame. New `raft_route_test` case covers this.

### Isolated node flashes 2 Hz, not failure-mode 4 Hz
- **Seen:** with both of N2's jumpers pulled, N2 flashed 4 Hz while N3 flashed
  the cockpit's 2 Hz hazards. N2 had lost N1's heartbeat only because N2
  itself was cut off, and `blink.c` treated any N1 loss as the cockpit down.
- **Changed:** failure mode (4 Hz) now requires N1 silent while this node
  still hears a bus. A zone that is ISOLATED, or has both buses degraded,
  flashes 2 Hz hazards. Its phase stays on global time (holdover), so it
  should stay in step with the other zone.

### Rest of the lab: rejoin, replay, snapshots, random leader, events, debug
- **Pi back (checkoff 7):** the default leader listens 250 ms before taking
  term 1, and joins a running cluster as a follower instead. Each node prints
  every committed entry once, marked `(replayed)` if it arrived already
  committed. The Pi's `raft_seen.log` stops entries shown before a restart
  from appearing again.
- **Log size:** a 256-entry RAM window with snapshot compaction and
  InstallSnapshot (an APPEND with entry count 0xFF, no new CAN ID) replaces
  the 64-entry log that rejected everything once full. `log_size` = 30,000
  lifetime entries. `log=` is now a running hash, so it still covers
  compacted entries.
- **COCKPIT_ERROR on cockpit loss:** STM32 clients report it with `NODE_DOWN
  N1`. The leader accepts it only if it has lost N1 too, and the STM32 drops
  its report when N1 returns.
- **EVENT entries:** the test button logs `APPLICATION` (1 = test fault on,
  2 = off) from the node that was pressed (checkoff 5).
- **Debug:** `RAFT_DEBUG` prints election and append messages as `RAFT ...`
  event lines.
- **Random leader (checkoff 8):** `LEADER=random` and `SEED=` build options
  for the Pi Makefile and the STM32 CMake. The boot election waits until
  every node is heard; `SEED=408` makes the cockpit win.
- **Tests:** `make -C tests` runs everything. New: boot listen (fresh and
  rejoin), 296 entries through the 256-entry window with snapshot catch-up
  and replay flags, cockpit-error acceptance, `raft_client_forget()`, an
  isolated leader changing the term exactly once, the snapshot codec, and
  `raft_random_test.c` (seed 408: N1 wins term 1).
- **Unverified:** none of this has run on hardware, and the STM32 code is only
  stub-compiled.

### Response-time statistics commented out
- **STM32s:** `WCET_MEASURE` and `JITTER_MEASURE` are 0 in `lab3_config.h`
  (the old `1` lines are left commented), so every probe compiles away. The
  WCET and J tables in `report.c` are commented out.
- **Pi:** the UDP->CAN latency line (mean/max) and the J table (max/mean),
  with their recording, are commented out in `cockpit.c`. The `J_*` tags
  stay, because `send_on_j()` calls still pass them.
- To restore them, uncomment those lines and set the two STM32 switches
  back to 1.

### Blinkers and hazards from global time; Zephyr system time
- **Blinkers:** N2 and N3 time FL/FR/RL/RR from `global_gettime()` through
  `shared/blink_phase.h`: on for the first half of each period, aligned to
  whole global seconds. Each lamp edge is a timed wake-up, no longer a 10 ms
  poll or a reaction to a BLINK frame. Front and rear therefore differ only by
  the sync error and wake-up latency, not by BLINK delivery.
- **Rates:** turn signals 1 Hz, hazards 2 Hz, failure-mode hazards (N1 lost)
  4 Hz. These were 1 Hz everywhere before.
- **Cockpit:** sends BLINK on each global-time lamp edge and on mode changes,
  and at least every 500 ms. A mode change no longer restarts the phase, so
  the first toggle after a change can come sooner than 500 ms. A new Pi test
  checks that hazard toggles are 250 ms apart.
- **System time:** follower STM32s call Zephyr's
  `sys_clock_settime(SYS_CLOCK_REALTIME)` after each REF they use. The SYNC
  line adds `sys=` and `ratio=`.
- **Tests:** `tests/blink_phase_test.c`. On the Pi, BLINK and UDP-timeout
  checks fail now and then on this code and on the committed code (1 run in
  6), when the test's simulated wheel stalls for more than 90 ms.
- **Unverified:** the STM32 build, and FL/RL alignment on the scope.
- **Zephyr 4.4.99:** `<zephyr/posix/time.h>` is gone, and the libc's
  `<time.h>` does not declare POSIX `clock_settime()`/`clock_gettime()`
  (implicit-declaration errors on the Mac build). The native
  `sys_clock_settime()`/`sys_clock_gettime()` from `<zephyr/sys/clock.h>`
  replace them, so `CONFIG_POSIX_TIMERS` is no longer set.

## 2026-11-12

### `4d2fa9f` Heartbeat may wait 0.5 ms for a TX buffer
- **Changed:** a node's own heartbeat waits up to `HB_TX_WAIT_US` (500 µs)
  for an MCP2515 TX buffer instead of `K_NO_WAIT`, only on a bus that is not
  degraded. CTRL, FAULT, BLINK, and relayed frames are unchanged.
  `queue_on()` in `canbus.c` takes the wait as a parameter.
- **Why:** the Zephyr MCP2515 driver frees a buffer slightly after its TX
  callback, so a `K_NO_WAIT` heartbeat could find none free while at most two
  frames were counted in flight. N2's heartbeat on bus B arrived 157/250
  times with a 180 ms gap.
- **Result:** every heartbeat on both buses (10 s capture: N2 500/500 on A and
  B, N3 500/500 and 499/499). N3 shows `full=0`; N2 shows `full` only on bus
  B, from relayed copies.

### `7a5b855` Raft sends are fail-safe; transmit debug counters
- **Changed:** a bulk Raft fragment waits up to 1 ms
  (`RAFT_TX_RESERVE_WAIT_US`) for a spare TX buffer, then is sent anyway with
  the old 2 ms wait, counted as `late`. It is never skipped. N2's relay does
  the same (`raftlate`). Each report bus line adds `tx q=` (frames
  `can_send()` accepted) and `infl N max M late L`.
- **Why:** the first version skipped a fragment after a 3 ms wait. Only
  fragment 0 of every message reached the bus (0 of 109 multi-frame messages
  complete), so N2 sat as a candidate at term 885 while N3 led at term 351.
- **Result:** 100/100 APPEND and APPEND_ACK messages complete; N2 and N3 agree
  on term and leader. `q` matches `ok` and `infl` is 0 when idle, so the
  in-flight count is accurate.

### `ead5b3e` MCP2515 SPI at 8 MHz request (5.25 MHz actual)
- **Changed:** `spi-max-frequency` from 4 MHz to 8 MHz in both STM32 overlays.
  SPI1 on the F401 runs from the 84 MHz APB2 clock with power-of-two dividers,
  so the real clock goes from 2.625 MHz to 5.25 MHz.
- **Why:** N2 relays about 190 frames/s through one SPI bus shared by both
  MCP2515s; its TX completions fell behind and its heartbeats were dropped.
- **Result:** N2's heartbeats on bus A went from 388/500 to 497/500 with no
  CAN errors at the new clock. Bus B still lost about 8% until `4d2fa9f`.

### `39b3912` Keep a TX buffer free for Lab 3 frames
- **STM32:** count frames in flight per bus from `can_send()` to the TX
  callback; bulk Raft fragments may use at most 2 of the 3 MCP2515 buffers
  (`RAFT_TX_MAX_INFLIGHT`). N2's relay applies the rule to relayed fragments
  and keeps forwarding CTRL and FAULT while one waits. N3's `relay.c` now
  matches N2's (it is compiled out on N3).
- **Pi:** Raft frames go out on a separate TX-only socket per bus with
  `SO_PRIORITY` 2 (pfifo_fast band 2); the Lab 3 socket uses 6 (band 0), so
  CTRL and HB are dequeued first. `can_rx` ignores N1's own Raft frames.
  `can_up.sh` raises `txqueuelen` from 10 to 32.
- **Why:** an empty APPEND is 4 fragments back to back; they filled all three
  MCP2515 buffers and the heartbeat, sent with `K_NO_WAIT`, was dropped
  (N3: 1-3 per second).
- **Note:** its skip-on-timeout rule broke Raft; fixed in `7a5b855`.

### `88ab415` Stop the CLIENT_REPLY storm; heartbeats outrank bulk Raft
- **CAN IDs:** REF stays at `0x040`-`0x042`. Votes, appends, and client
  messages move from `0x044`-`0x05A` to `0x140`-`0x156`, above the heartbeats
  (`0x100`-`0x102`). STM32 receive filters stay at six: BLINK and FORCE share
  one. `raft_id_slot()` keeps the transport's reassembly table compact.
- **Client pacing** (`shared/raft_agent.c`): link, node, and cockpit reports go
  through `raft_client_observe()` and are logged only after holding 50 ms
  (loss) or 100 ms (recovery). A newer state replaces a queued one for the
  same link or node. After `REJECTED` the client waits 1 s instead of 100 ms.
  The leader stays silent while another entry is in flight and answers its own
  client locally instead of over CAN.
- **Why:** the 64-entry log filled, the leader rejected every update, and
  clients resent every 100 ms (about 90 rejected replies/s per bus). That
  traffic outranked heartbeats, gaps reached 60-80 ms, DEG flickered, and each
  flicker added two more log reports. The term had reached 1389.
- **Tests:** host tests cover the ID order and filter masks, debounce,
  coalescing, the 1 s backoff, and the leader rejecting its own request
  without sending a frame.

### `2c9625f` Initial Raft
- Shared portable Raft core in `shared/` used by the Pi and both STM32s:
  `protocol.h` (CAN IDs), `raft_wire.h` (message codecs), `raft_transport.c`
  (5-byte fragments and reassembly), and `raft_agent.c` (roles, terms,
  elections, one-entry APPEND, commit on majority, network graph).
- Default startup: N1 leader in term 1, REF every 20 ms, empty APPEND every
  100 ms; followers elect after 3 missed REFs plus 0-39 ms of random delay.
  `RAFT_DEFAULT_LEADER -1` starts with an election instead.
- Lab 3 heartbeat monitors report CANLINK and NODE changes; the Pi reports
  COCKPIT_OK and COCKPIT_ERROR. Each node prints its term, log, and graph once
  a second.
- Host tests: `tests/raft_wire_test.c`, `raft_transport_test.c`,
  `raft_agent_test.c`.

## Known issues
- **Log capacity:** the working log holds 64 entries in RAM and is lost on
  reset. When full, the leader rejects every update. Needs a ring buffer with
  snapshots.
- **N2 relays everything:** about 70 relayed Raft frames/s are duplicates when
  all links are up, and relayed copies on bus B still fail with `full`.
  Relaying Raft frames only when the sender is not heard on the other bus
  would remove that load.
- **Elections:** no Pre-Vote or CheckQuorum, and a restarted cockpit claims
  term-1 leadership with an empty log, so the term can climb (checkoff step 10).
- **Not started:** clock synchronization (`global_gettime()`, drift), PPS,
  blinkers on global time, LEADER and SYNC indicators, failure-mode hazards,
  graph-based routing.
