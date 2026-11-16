## Message types

There are seven Lab 5 message types in our protocol. Think of them as two conversations: nodes agreeing on a log, and the leader sharing time. CAN IDs and byte layouts are in `docs/lab5_protocol.md`.

| Message | Sent by | What it does |
|---|---|---|
| `REF` | Current leader, every 20 ms | Says "I'm still the leader" and supplies the master clock. Followers use it for time sync and to know whether the leader is still present. |
| `REQ_VOTE` | A node seeking leadership | Starts an election after it misses enough leader references. It includes its term and how up to date its log is. |
| `VOTE_REPLY` | Each other node | Grants or denies that candidate's vote. Two votes out of three make a leader. |
| `CLIENT_UPDATE` | Any node's Raft client | Reports an observation, such as "rear CAN A went down," to the leader. It does **not** directly change the agreed network state. |
| `CLIENT_REPLY` | The node contacted by the client | A follower says "send that to this leader." The leader eventually says "your update was committed" after replication succeeds. |
| `APPEND` | Leader | Sends a log entry to followers. It includes the term, where the entry belongs, and the current commit point. An *empty* `APPEND` carries no new entry but still checks and maintains log agreement. |
| `APPEND_ACK` | Follower | Says whether the `APPEND` matched its log. The leader uses these replies to replicate an entry to a majority before committing it. |

Here is one complete example: rear CAN A fails → rear client sends `CLIENT_UPDATE` → leader sends an `APPEND` containing `CANLINK_N3_A_DOWN` → a follower sends `APPEND_ACK` → leader commits the entry and sends `CLIENT_REPLY` → nodes apply it to their network graphs.

## Steps

- **Define the protocol.** Assign CAN IDs and payload formats for Raft votes, log replication, membership events, and `REF`. Decide how entries larger than one CAN frame are carried. Keep the driving `CTRL` path fast. **Check:** all three nodes interpret the same frames the same way.

- **Add a Raft agent and client to each node.** The agent manages leadership, terms, and the log. The client reports local link and node changes and asks for network state. A client that contacts a follower must be directed to the leader. **Check:** all three agents start and can exchange messages.

- **Implement default startup.** Give every node the same configuration. Start in term 1 with the cockpit as leader, sending `REF` every 20 ms, with no election at boot. Support the handout’s random-leader startup mode too. **Check:** all nodes display the same leader and term.

- **Implement leader failure and elections.** After three missed leader heartbeats, plus the specified random timeout, followers request votes. A majority elects one leader. A broken CAN link alone must not trigger an election if the leader remains reachable. **Check:** removing the Pi produces one STM32 leader within one second.

- **Implement log replication.** The leader orders events, sends them to followers with `APPEND`, commits them after a majority accepts them, and brings a returning node up to date. Include the empty `APPEND` traffic required by the checkoff. **Check:** all nodes show the same committed entries at the same indexes, with no duplicates.

- **Build the network state machine.** Apply committed `COCKPIT_OK/ERROR`, `NODE_UP/DOWN`, and `CANLINK_UP/DOWN` events to a graph of the nodes and their six bus links. Provide `getNetwork()`. **Check:** each node reports the same graph after a link is removed and restored.

- **Connect Lab 3 fault detection to the Raft clients.** Reuse the existing heartbeat and degraded-bus detection to report real changes to the leader. Record each change once. **Check:** pulling one CAN link creates one matching membership change on every node without changing the leader.

- **Make routing use the graph.** Use each node’s committed network state to choose which bus can carry a message and when the front node must relay it. Preserve duplicate suppression. **Check:** the handout’s two-link “hop” still carries driving commands and logged events end to end.

- **Synchronize clocks.** Put the leader’s time in `REF`. On each follower, estimate CAN delivery delay and clock drift, then implement `global_gettime()` and the required drift printout. **Check:** all nodes show synchronization and continue receiving `REF` through a relay.

- **Synchronize visible outputs.** Add a PPS GPIO output on each node, aligned to whole seconds of global time. Drive front and rear blinker timing from global time. Add the required leader and sync indicators or test points. **Check:** front and rear blinker edges are within 1 ms, and PPS edges line up on the scope.

- **Handle Pi failure and return.** When the cockpit goes down, enter the safe state and brake within 100 ms. The elected STM32 takes over `REF`, and synchronized hazards run at the required failure rate. When the Pi returns, it rejoins as a follower and replays missed entries in order. **Check:** leadership stays with the STM32 after the Pi returns, with no duplicate replayed events.

- **Run the full checkoff and write the report.** Repeat the Lab 3 behaviors, then test link failure and recovery, the hop, Pi failure, random leader, relayed sync, isolation, and the test button. Capture the logs and scope measurements. The report needs the GitHub link and short explanations of drift correction, the Raft state machine, and the Raft client’s effect on routing.
