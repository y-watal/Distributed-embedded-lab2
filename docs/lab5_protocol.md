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
