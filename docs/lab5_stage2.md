# Lab 5 step 2: agent and client status

Each of the three active node builds now starts a Raft agent. The Pi and both
STM32 applications use the same portable `shared/raft_agent.c` and
`shared/raft_transport.c`. The Pi runs its agent in a pthread. Each STM32 runs
its agent in a lower-priority Zephyr thread. CAN reception only queues Raft
frames, so it does not run log work on the brake path.
The N2 relay gives CTRL and FAULT frames an urgent queue ahead of Raft
fragments.

At startup the shared config identifies N1 as leader in term 1. The agents
track role, term, vote, log indexes, commit index, and the graph built from
committed events. Clients queue node and link changes observed by the existing
Lab 3 heartbeat monitors. The Pi also reports cockpit OK or error as wheel
packets arrive or stop. A client sends `CLIENT_UPDATE` to its known leader.
A follower contacted as leader sends `CLIENT_REPLY REDIRECT` with its leader
hint. The client retries the same request ID at that leader.

The working leader appends one event at a time, sends `APPEND`, and commits it
after one follower acknowledges, which is a majority of the three-node
cluster. It then sends the committed index to followers and acknowledges the
client. Repeated reports of an already applied membership state do not add
another log entry. Every node offers `raft_node_get_network()` and prints a
compact network state once per second. `?` means not yet known, `U` up, and
`D` down.

This is a working step-2 log limited to 64 volatile entries so it fits in the
STM32 RAM. The handout's 30,000-entry log capacity and recovery across reset
still require a storage design. Default startup now sends periodic `REF` and
empty `APPEND`. After three missed `REF` periods plus 0–39 ms of jitter,
followers request votes. Complete follower catch-up, synchronized time,
graph-based routing, and failure-mode behavior remain later steps.

## Hardware smoke check

Build and run the Pi program and flash both STM32 images from the `lab5`
branch. Once the two CAN interfaces and three nodes are up:

1. Each console should report term 1, leader N1, and its own role.
2. `candump -c any` on the Pi should show client updates (`0x151`, `0x152`),
   leader appends (`0x148`), follower acknowledgements (`0x14d`, `0x14e`),
   and client replies (`0x154`). These arrive as multiple frames per message.
3. Each console should show its log and committed indexes rising and its
   network states converging as startup node and link reports commit.
4. Pull one CAN link and restore it. The owner should report its link down and
   up, and the changes should appear in the network state on all nodes.

Expect `REF` every 20 ms and empty `APPEND` every 100 ms with the default
leader. Removing the leader now triggers a vote after missed `REF` messages.
The step-2 host tests cover client
redirection, majority acknowledgement, committed graph agreement, and CAN
fragment reassembly. The hardware smoke check is still needed because the
Pi application cannot be fully built or run on macOS.
