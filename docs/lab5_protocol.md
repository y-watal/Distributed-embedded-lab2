# Lab 5 CAN wire protocol

This document defines the wire format for the required Lab 5 messages. The
machine-readable definitions and codecs are in `shared/protocol.h` and
`shared/raft_wire.h`. The three C builds include those same headers. The step 2
agent and client use these formats for membership updates and log replication.
Periodic `REF` and empty `APPEND` are enabled for startup. Missing three REF
periods plus 0–39 ms of jitter triggers an election. Durable full-size logs
are a later step.

## CAN IDs and priority

All frames use standard 11-bit CAN IDs and DLC 8. The low two bits of each
Lab 5 ID identify the sender: 0 is N1 cockpit, 1 is N2 front, and 2 is N3 rear.
The fourth ID in each block is unused. A leader can therefore be any node.

| IDs for N1, N2, N3 | Message | Purpose |
|---|---|---|
| `0x040`–`0x042` | `REF` | Leader presence and master time every 20 ms |
| `0x140`–`0x142` | `REQ_VOTE` | Candidate requests a vote |
| `0x144`–`0x146` | `VOTE_REPLY` | Voter grants or denies a candidate |
| `0x148`–`0x14A` | `APPEND` | Leader heartbeat or one log entry |
| `0x14C`–`0x14E` | `APPEND_ACK` | Follower confirms or rejects an append |
| `0x150`–`0x152` | `CLIENT_UPDATE` | Client reports a local membership or application event |
| `0x154`–`0x156` | `CLIENT_REPLY` | Leader confirms a commit or follower redirects |

A lower CAN ID wins arbitration, so the IDs follow deadlines:

| Priority | IDs | Messages | Reason |
|---|---|---|---|
| 1 | `0x010`–`0x022` | `CTRL`, `FAULT_Nx` | Brake within 100 ms |
| 2 | `0x040`–`0x042` | `REF` | One frame at 50 Hz; arbitration delay is time-sync error |
| 3 | `0x100`–`0x102` | `HB_Nx` | 44 ms degraded-bus deadline |
| 4 | `0x140`–`0x156` | Votes, appends, client messages | Fragmented, retried after 100 ms or more |
| 5 | `0x200`, `0x300` | `BLINK`, `FORCE` | Unchanged from Lab 3 |

The bulk Raft block originally sat at `0x044`–`0x05A`, below the heartbeats.
A retry storm of rejected client updates then delayed heartbeats past 44 ms
and made the degraded flags flicker. Keeping the bulk block above `HB` means
a burst of fragments can no longer win arbitration over a heartbeat.
Arbitration only orders frames already waiting in different nodes. Inside one
node the transmit path also keeps Raft from delaying Lab 3 frames:

- **STM32:** the MCP2515 has three TX buffers and Lab 3 frames are sent with
  `K_NO_WAIT`. Bulk Raft fragments prefer to hold at most two
  (`RAFT_TX_MAX_INFLIGHT`), counted in `canbus.c` from `can_send()` to the TX
  callback, so a heartbeat, CTRL, or FAULT finds one free. A fragment waits up
  to 1 ms for that (`RAFT_TX_RESERVE_WAIT_US`), then is sent anyway with the
  old 2 ms wait for any buffer and counted as `late`. It is never skipped: a
  first version skipped fragments after a 3 ms wait, every multi-frame
  message lost a fragment, and N2 and N3 split (N2 candidate at term 885,
  N3 leader at term 351). N2's relay applies the same rule to relayed
  fragments and keeps forwarding CTRL and FAULT while one waits (`raftlate`).
  REF is one frame and may wait for any buffer. The reserve alone did not
  stop heartbeat losses: the Zephyr MCP2515 driver frees a TX buffer slightly
  after it calls the TX callback, so a `K_NO_WAIT` send could find none free
  while at most two frames were counted in flight. A node's own heartbeat
  therefore waits up to 0.5 ms for a buffer (`HB_TX_WAIT_US`) on a bus that is
  not degraded; CTRL, FAULT, and relayed frames stay `K_NO_WAIT`. Each bus line of the report
  prints `tx q=` (frames `can_send()` accepted, which should equal `ok` +
  `err`) and `infl N max M late L` (frames in flight now, the most seen, and
  late Raft sends) to show whether the in-flight count tracks the hardware.
- **Pi:** the `mcp251x` driver sends one frame at a time from the interface
  queue. Raft uses its own TX-only socket with `SO_PRIORITY` 2 (pfifo_fast
  band 2); the Lab 3 socket uses 6 (band 0), so CTRL and HB are dequeued
  ahead of any waiting fragment. `can_up.sh` raises `txqueuelen` from 10 to
  32 so a burst cannot make a CTRL or HB `write()` fail.

Each STM32 still uses six MCP2515 receive filters: `BLINK` and `FORCE` share
one (they differ only in bit 8), `REF` uses `0x040`/`0x7FC`, and the bulk
block uses `0x140`/`0x7E0`. The future Raft sender must use its own bounded queue and must
never block the Pi's wheel-to-`CTRL` path. Arbitration does not preempt a frame
that is already on the wire.

The Lab 3 message layouts and four-bit sequence convention remain unchanged.
The new messages use the formats below and have a separate receive, duplicate
suppression, relay, and send path. The Pi and STM32 receive filters now accept
the new ID block.

## `REF`: one frame, no fragmentation

| Bytes | Field | Format |
|---|---|---|
| 0–3 | Master clock, whole seconds in the leader's global-time epoch | `uint32` little endian |
| 4–5 | Fraction of a second | `uint16` little endian, units of 1/65536 second |
| 6–7 | Reference cycle number | `uint16` little endian, wraps modulo 65536 |

This gives about 15.3 microsecond timestamp resolution. The initial leader
chooses the epoch, which can be its local clock at startup. Later leaders keep
the synchronized global clock rather than resetting the epoch. The leader's node ID is
in the CAN ID. Followers accept `REF` only from the current leader and reject
old or repeated cycle numbers. The cycle number is not a Raft term. Terms are
carried in the Raft vote and append messages. When leadership changes, the new
leader continues the agreed global time and chooses its next cycle number.

### Time synchronization from `REF`

`shared/timesync.c` holds the model; the STM32s wrap it in `gtime.c` and the
Pi in `node1_pi/gtime.c`. All times are microseconds.

- **Stamping.** The leader writes `global_gettime()` into bytes 0–5 per bus,
  just before the frame goes to the controller (`canbus_send_raft()` on an
  STM32, `send_on_j()` on the Pi). The Pi sends `REF` on its Lab 3 socket
  (pfifo_fast band 0), not the bulk Raft socket. The A and B copies therefore
  carry slightly different stamps and are no longer byte-identical.
- **Receiving.** A follower uses only `REF` from the node its Raft agent calls
  leader, timestamped in `rx_cb` (STM32) or right after `read()` (Pi). The
  first copy of each cycle number is used; the other bus's copy or a relayed
  copy counts as `stale`.
- **Offset.** `measured = L + TIMESYNC_REF_DELAY_US` (350 µs: controller load,
  one 8-byte frame at 500 kbit/s, receiver interrupt and SPI read). The
  error against the running model is halved and clamped to ±50 µs per `REF`,
  so a new leader's clock is slewed in and never stepped by more than 50 µs.
  An error over 2 ms (first sync, or a rebooted master) is stepped.
- **Drift.** `(L2 − L1) / (F2 − F1)` over at least 1 s of `REF` from one
  master, averaged over about four windows, in ppb. Between `REF`s,
  `global = base_global + dl·(1 + drift)`, so holdover keeps the rate.
- **Leader.** A leader keeps running its own model, so an elected follower
  continues the agreed time without a step. A node that has never synced
  (the Pi at boot) uses its local clock: the epoch is its boot.
- **Relay.** N2 restamps a leader's `REF` with its own global time just before
  forwarding it, so the relay delay does not appear as clock offset on the far
  side.
- **SYNC.** A node is synchronized if it leads, or if it heard its master within
  1 s (`TIMESYNC_HOLDOVER_US`).
- **PPS.** At every whole global second, a thread (priority 6 on the STM32s)
  sleeps until 0.4 ms before the edge, then spins onto it. It drives `TP_PPS`
  high for 100 ms and records a snapshot, which the 1 s report prints as:

  `SYNC: global=S.us local=S.us master=N1 synced=1 drift=±X.XXXppm err=Eus corr=Cus driftcorr=Dus refs=R stale=S steps=T`

  `err` is the offset the last `REF` measured, and `corr` is how far that
  `REF` moved the clock. `driftcorr` is the rate correction applied since the
  last `REF`. `refs` and `stale` are per-second counts.

- **System time.** After each `REF` it uses, a follower STM32 also sets
  Zephyr's realtime clock with `sys_clock_settime(SYS_CLOCK_REALTIME)`, the
  kernel call under POSIX `clock_settime()`, as handout 6.1 asks. It has
  kernel-tick resolution;
  `global_gettime()` stays the precise source. The `SYNC:` line prints both
  (`sys=`), plus the drift ratio `(L2 − L1) / (F2 − F1)` itself (`ratio=`).
- **Blinkers.** A lamp is lit during the first half of each period of global
  time, with periods aligned to whole global seconds (`shared/blink_phase.h`).
  FL, RL and PPS therefore switch at the same global instant. `BLINK` frames
  only choose the mode; N2 and N3 ignore the phase bit, and the cockpit sets it
  from the same function. The rates are:

  | Mode | Rate | Decided by |
  |---|---|---|
  | Left / right turn signal | 1 Hz (Lab 3 R2.4) | cockpit `BLINK` |
  | Hazards, cockpit up | 2 Hz | cockpit `BLINK` HAZARD; this node isolated (ISOLATED or both buses degraded); `BLINK` stale (2.6 s) |
  | Hazards, failure mode | 4 Hz | N1's heartbeat lost while this node still hears the bus: the cockpit is down |

The PPS, SYNC, LEADER and REF_RX pins are PC10, PC11, PC12 and PD2 on both
STM32s (morpho CN7 pins 1-4), and GPIO12, 21, 17 and 27 on the Pi (header
pins 32, 40, 11 and 13). See docs/lab3_pin_map.md, "Lab 5 pins". On the
Pi, only `cockpit.c` maps `/dev/gpiomem`, so `gtime.c` and `raft_node.c` drive pins through
`pi_tp_set()` and `pi_tp_toggle()`.

## Other messages: fragmentation

Every non-`REF` body is a byte string encoded below. It is split into five-byte
pieces. Each CAN frame has this envelope:

| Byte | Meaning |
|---|---|
| 0 | Transfer ID, incremented for each new message of this CAN ID |
| 1 | Fragment number, starting at zero |
| 2 | Total body length in bytes, at most 255 |
| 3–7 | Five successive body bytes, with zero padding on the last fragment |

The receiver reassembles by `(CAN ID, transfer ID)` using the body length to
know the fragment count. It accepts out-of-order fragments and ignores
duplicate copies from bus A, bus B, or a relay. An incomplete transfer expires
after a short timeout. A Raft retry uses a new transfer ID. Every copy of one
fragment on the two buses is byte-identical. The old Lab 3 four-bit sequence
number is **not** a log index or a Raft transfer ID.

All multibyte integers in these bodies are little endian and unsigned. Terms,
log indexes, and request IDs are 32 bits. An index is a lifetime sequence
number, independent of the configured 30,000-entry storage capacity.

| Body | Length | Fields in byte order |
|---|---:|---|
| `REQ_VOTE` | 12 | term `u32`, last log index `u32`, last log term `u32` |
| `VOTE_REPLY` | 6 | term `u32`, candidate node `u8`, granted `u8` (0 or 1) |
| Empty `APPEND` | 18 | target node `u8`, term `u32`, previous index `u32`, previous term `u32`, leader commit index `u32`, entry count `u8` = 0 |
| One-entry `APPEND` | 26 | the same 18 bytes with entry count = 1, then entry term `u32` and event `4 bytes` |
| Snapshot `APPEND` | 26 | the same 18 bytes with entry count = `0xFF`; previous index/term = the last index/term the snapshot covers; then network state `u32` (20 bits) and committed-log hash `u32` |
| `APPEND_ACK` | 10 | term `u32`, match index or retry index `u32`, target leader `u8`, success `u8` (0 or 1) |
| `CLIENT_UPDATE` | 9 | target leader `u8`, client request ID `u32`, event `4 bytes` |
| `CLIENT_REPLY` | 11 | target client `u8`, request ID `u32`, status `u8`, leader hint `u8`, committed index `u32` |

The sender is already known from the CAN ID. `APPEND` carries at most one entry,
so the entry's log index is `previous index + 1`. The target may be a node ID
or `0xFF` for all. A follower that is not leader can answer `CLIENT_UPDATE`
with `CLIENT_REPLY` status `REDIRECT` and the leader hint. A leader replies
`COMMITTED` only after a majority has the entry. Empty `APPEND` is sent every
100 ms during default-leader operation, while `REF` continues every 20 ms.

An empty `APPEND` occupies four CAN frames. A one-entry `APPEND` occupies six.
The fragments are reassembled into one logical message before the Raft agent
checks its term, previous index, and commit index. The protocol can carry a
body up to 255 bytes if a later message needs more than one entry, although
the current `APPEND` format deliberately sends one entry at a time.

## Log events and membership

Each event is four bytes: `type`, `node`, `bus`, `value`. The defined types are
`COCKPIT_OK`, `COCKPIT_ERROR`, `NODE_UP`, `NODE_DOWN`, `CANLINK_UP`,
`CANLINK_DOWN`, and `APPLICATION`. Node is 0–2. Bus is 0 for A, 1 for B,
or `0xFF` when the event is not about a link. Value is zero except for an
application event code. For `APPLICATION`, node is the originating node.

## Startup, log storage and the log display

**Default startup.** The configured leader (the cockpit) listens for
`RAFT_BOOT_LISTEN_MS` (250 ms) before taking term 1. If it hears another
node's `REF` or `APPEND`, a cluster is already running, so it joins that
leader as a follower. This is the Pi coming back after an STM32 took over
(checkoff 7), and LEADER stays on the STM32. The other nodes boot as
followers of the cockpit in term 1 and start their election timers at its
first `REF`, as before.

**Random leader** (`LEADER=random`; Pi: `make LEADER=random`, STM32:
`west build ... -- -DLEADER=random`). Every node starts with no leader. The
boot election timer starts only once the node has heard every other node,
so the Pi's ~30 s boot doesn't decide the winner. `SEED=<n>` makes each
node's timeouts reproducible. `SEED=14` (`RAFT_SEED_N2_WINS`) makes N2
win and `SEED=112` makes N3 win: checkoff 8's "an STM32 is leader".
`SEED=408` (`RAFT_SEED_PI_WINS`) gives the cockpit a 60 ms first timeout
against 95 ms on both STM32s, so the Pi wins (checkoff 8's second part). After that, elections follow missed `REF`s as in default mode.

**Log storage.** `log_size` is `RAFT_LOG_CAPACITY` = 30,000 lifetime
entries; beyond that the leader answers `REJECTED`. Each node keeps the
newest `RAFT_LOG_WINDOW` (256) entries in RAM; 30,000 entries of 8 bytes
would not fit in the STM32's 96 KB. Older applied entries are folded into a
snapshot: the network state, the last index and term, and the running FNV
hash of every committed entry. If a follower needs an entry the leader has
compacted, the leader sends a snapshot `APPEND`, then the entries still in
RAM. The `log=` hash on the `RAFT:` line covers every committed entry,
compacted ones included, so it still compares across nodes.

**COCKPIT_ERROR / COCKPIT_OK.** The Pi reports its own state: OK while
wheel data arrives, ERROR without it or with its test fault. When an STM32
stops hearing N1's heartbeat it reports `COCKPIT_ERROR` (with `NODE_DOWN
N1`), so the new leader logs the failure mode. The leader accepts it only if
it has lost N1 too. When N1's heartbeat returns, the STM32 drops its report
and the cockpit's own report decides.

**EVENT entries.** The test button on any node logs an `APPLICATION` entry
from that node: value 1 when the test fault engages, 2 when a double press
clears it. During the hop, pressing N2's test button sends N2's EVENT to the
Pi and on to N3's log through the relay (checkoff 5).

**Log display.** Every node prints each committed entry once, as it is
applied: `MEMBER #n term t NODE_/CANLINK_...`, `EVENT #n ...` or `COCKPIT #n
...`. Entries that were already committed when the node received them
(catch-up after an outage) are marked `(replayed)`. The Pi records what it
has shown in `raft_seen.log` in its working directory. After a restart it
does not repeat entries it showed before going down: the held events appear
once, marked replayed, then live events (checkoff 7). A fresh cluster, where
the Pi takes term 1 at boot, clears the file. The status block prints only
the newest 16 entries (`RAFT_PRINT_TAIL`).

**Debug.** With `RAFT_DEBUG` (the handout's `Debug` key, default on), every
node prints `RAFT ...` event lines: pre-votes, vote requests and replies,
leadership, each entry the leader appends and sends, each entry a follower
stores, commits, and snapshots. Empty APPENDs and REF are not printed.

## Client pacing

The Lab 3 monitors still flip `DEG` and peer-lost immediately for local
behaviour. What goes into the replicated log is debounced in
`raft_client_observe()`:

- A cockpit error, node loss, or link loss is reported only after it has
  persisted for 50 ms beyond detection (`RAFT_REPORT_DOWN_HOLD_MS`). With the
  44 ms `DEG` threshold, a link is logged down after about 94 ms of silence.
- A recovery is reported after 100 ms of stable state (`RAFT_REPORT_UP_HOLD_MS`).
- A flicker shorter than the hold is never logged.
- A newer state for the same cockpit, node, or link replaces a queued one.
- An unanswered `CLIENT_UPDATE` is resent every 100 ms. After `REJECTED`
  (working log full) the client waits 1 s before resending.
- While another entry is in flight, the leader does not answer; the client's
  normal resend covers it. The leader's own client is answered in place,
  never over CAN.

`MEMBER` and `EVENT` are log or console labels used by the checkoff, not
additional CAN IDs in this design. A client sends the four-byte event in
`CLIENT_UPDATE`. The leader stores it in the log and replicates it in
`APPEND`. The state machine applies only committed entries, in log-index order.
