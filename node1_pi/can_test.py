#!/usr/bin/env python3
"""Lab 3 CAN test for the Pi (N1): heartbeats, dedup, HB_MON, test points.

Sends HB_N1 (0x100) on can0 (bus A) and can1 (bus B) every 20 ms, with the
Lab 1 heartbeat layout and a per-ID 4-bit sequence counter in byte 0. Every
frame from N2/N3 is counted per bus; copies of one (ID, seq) - bus A, bus B and
N2's relayed copy - are collapsed into one "accepted" frame. Once a second:

    [N1 12.0s] NORMAL | peers: N2 ok N3 ok | dup 150 echo 50
      A can0: ERROR-ACTIVE DEG 0 | tx ok=50 err=0 | rx: 0x101 x50 0x102 x50
      B can1: ERROR-ACTIVE DEG 0 | tx ok=50 err=0 | rx: 0x100 x50 0x101 x50 0x102 x100
      accepted: 0x101 x50 0x102 x50

With N2 relaying, N2 forwards one copy of every N1/N3 frame to the other bus,
so one bus carries N3's heartbeat twice and N1's own heartbeat comes back as
"echo" (which bus depends on which copy N2 sees first).

Test points (testpoints.py): CAN_TX_A/B per send; DEG_A/B on while no other
node's heartbeat was heard on that bus for 44 ms; ERR on until both other zones
have been heard, and while either is missing on both buses for 66 ms.

Run ./can_up.sh first. Needs python-can: sudo apt install python3-can
"""
import argparse
import json
import subprocess
import threading
import time

import can

from testpoints import TestPoints, walk

# Catalog (docs/lab3_design.md)
N1, N2, N3 = 0, 1, 2
SELF = N1
ID_HB = lambda n: 0x100 + n          # noqa: E731
ID_FAULT = lambda n: 0x020 + n       # noqa: E731
ID_CTRL, ID_BLINK, ID_FORCE = 0x010, 0x200, 0x300
SENDER = {ID_CTRL: N1, ID_BLINK: N1, ID_FORCE: N2,
          **{ID_HB(n): n for n in (N1, N2, N3)},
          **{ID_FAULT(n): n for n in (N1, N2, N3)}}
ZONE_BOOT, ZONE_SYS_ERROR, ZONE_NORMAL = 0, 1, 2
STATE_NAME = {ZONE_BOOT: "BOOT", ZONE_SYS_ERROR: "SYS_ERROR", ZONE_NORMAL: "NORMAL"}

# Thresholds, same as the Nucleos' lab3_config.h
HB_MAX_INTERVAL_S = 0.022
DEG_TIMEOUT_S = 2 * HB_MAX_INTERVAL_S   # 44 ms
FAIL_TIMEOUT_S = 3 * HB_MAX_INTERVAL_S  # 66 ms
DUP_WINDOW_S = 0.010
MONITOR_PERIOD_S = 0.005

BUSES = {"A": "can0", "B": "can1"}
PEERS = (N2, N3)

lock = threading.Lock()
rx_raw = {name: {} for name in BUSES}    # id -> copies received on that bus
accepted = {}                            # id -> [count, gaps]
seen = {}                                # id -> (seq, time) of the last accepted copy
counters = {"dup": 0, "echo": 0}
tx_stats = {name: [0, 0] for name in BUSES}  # [ok, err]
last_hb = {name: {n: None for n in PEERS} for name in BUSES}
deg = {name: True for name in BUSES}
peer_alive = {n: False for n in PEERS}
zone_state = ZONE_BOOT


def rx_loop(name, bus, stop):
    while not stop.is_set():
        msg = bus.recv(timeout=0.2)
        if msg is None or msg.is_error_frame or msg.dlc == 0:
            continue
        can_id, now = msg.arbitration_id, time.monotonic()
        sender = SENDER.get(can_id)
        with lock:
            rx_raw[name][can_id] = rx_raw[name].get(can_id, 0) + 1
            if sender == SELF:          # our own frame, relayed back by N2
                counters["echo"] += 1
                continue
            if sender in PEERS and can_id == ID_HB(sender):
                last_hb[name][sender] = now   # any copy counts as "alive"
            seq = msg.data[0] & 0x0F
            prev = seen.get(can_id)
            if prev and prev[0] == seq and now - prev[1] < DUP_WINDOW_S:
                counters["dup"] += 1
                continue
            acc = accepted.setdefault(can_id, [0, 0])
            acc[0] += 1
            if prev:
                acc[1] += (seq - prev[0] - 1) & 0x0F
            seen[can_id] = (seq, now)


def heard(name, node, now, timeout):
    t = last_hb[name][node]
    return t is not None and now - t <= timeout


def monitor(tp, state):
    """HB_MON: DEG_A/B from the 44 ms threshold, zone failure from 66 ms."""
    global zone_state
    now = time.monotonic()
    with lock:
        for name in BUSES:
            deg[name] = not any(heard(name, n, now, DEG_TIMEOUT_S) for n in PEERS)
        for n in PEERS:
            peer_alive[n] = heard("A", n, now, FAIL_TIMEOUT_S) or heard("B", n, now, FAIL_TIMEOUT_S)
        all_alive = all(peer_alive.values())
        if not state["booted"] and all_alive:
            state["booted"] = True
        zone_state = ZONE_BOOT if not state["booted"] else (ZONE_NORMAL if all_alive else ZONE_SYS_ERROR)
    tp.set("DEG_A", deg["A"])
    tp.set("DEG_B", deg["B"])
    tp.set("ERR", zone_state != ZONE_NORMAL)


def link_state(channel):
    try:
        out = subprocess.run(["ip", "-details", "-json", "link", "show", channel],
                             capture_output=True, text=True, timeout=1).stdout
        info = json.loads(out)[0]
        if info.get("operstate") == "DOWN":
            return "DOWN"
        data = info.get("linkinfo", {}).get("info_data", {})
        state = data.get("state", "?")
        berr = data.get("berr_counter")
        if berr:
            state += f" tec={berr.get('tx', '?')} rec={berr.get('rx', '?')}"
        return state
    except (OSError, ValueError, IndexError, subprocess.SubprocessError):
        return "?"


def fmt_counts(counts):
    if not counts:
        return " (nothing)"
    out = ""
    for can_id in sorted(counts):
        c = counts[can_id]
        if isinstance(c, list):
            out += f" 0x{can_id:03x} x{c[0]}" + (f" (gaps {c[1]})" if c[1] else "")
        else:
            out += f" 0x{can_id:03x} x{c}"
    return out


def report(start):
    with lock:
        head = (f"[N1 {time.monotonic() - start:.1f}s] {STATE_NAME[zone_state]} | peers:"
                + "".join(f" N{n + 1} {'ok' if peer_alive[n] else 'LOST'}" for n in PEERS)
                + f" | dup {counters['dup']} echo {counters['echo']}")
        rows = []
        for name, channel in BUSES.items():
            ok, err = tx_stats[name]
            rows.append((name, channel, int(deg[name]), ok, err, fmt_counts(rx_raw[name])))
            tx_stats[name] = [0, 0]
            rx_raw[name] = {}
        acc = fmt_counts(accepted)
        for c in accepted.values():
            c[0] = c[1] = 0
        counters["dup"] = counters["echo"] = 0
    print(head)
    for name, channel, d, ok, err, rx in rows:   # ip runs outside the lock
        print(f"  {name} {channel}: {link_state(channel)} DEG {d} | tx ok={ok} err={err} | rx:{rx}")
    print(f"  accepted:{acc}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--period-ms", type=int, default=20, help="heartbeat period (default 20)")
    ap.add_argument("--listen-only", action="store_true", help="receive only, send nothing")
    ap.add_argument("--no-walk", action="store_true", help="skip the start-up test point walk")
    args = ap.parse_args()

    if not args.no_walk:
        print("Test point walk:")
        walk()

    buses = {}
    for name, channel in BUSES.items():
        try:
            buses[name] = can.Bus(channel=channel, interface="socketcan")
        except (OSError, can.CanError) as e:
            print(f"bus {name} ({channel}) not available: {e}")
    if not buses:
        raise SystemExit("no CAN interface is up - run ./can_up.sh")

    tp = TestPoints()
    for p in ("ERR", "DEG_A", "DEG_B"):
        tp.set(p, 1)

    stop = threading.Event()
    threads = [threading.Thread(target=rx_loop, args=(n, b, stop), daemon=True)
               for n, b in buses.items()]
    for t in threads:
        t.start()

    period = args.period_ms / 1000.0
    start = time.monotonic()
    next_tx = next_monitor = start
    next_report = start + 1.0
    seq = 0
    mon_state = {"booted": False}

    try:
        while True:
            now = time.monotonic()
            if not args.listen_only and now >= next_tx:
                with lock:
                    b2 = (0x01 if deg["A"] else 0) | (0x02 if deg["B"] else 0)
                    b1 = zone_state & 0x0F
                # Lab 1 HB layout: byte 0 seq, 1 state, 2 degraded bits, 3 fault code
                msg = can.Message(arbitration_id=ID_HB(SELF), is_extended_id=False,
                                  data=[seq, b1, b2, 0, 0, 0, 0, 0])
                for name, bus in buses.items():
                    tp.toggle("CAN_TX_" + name)
                    try:
                        bus.send(msg, timeout=0)
                        ok = True
                    except can.CanError:
                        ok = False  # TX queue full (no ACK on the bus) or interface down
                    with lock:
                        tx_stats[name][0 if ok else 1] += 1
                seq = (seq + 1) & 0x0F
                next_tx += period
            if now >= next_monitor:
                monitor(tp, mon_state)
                next_monitor += MONITOR_PERIOD_S
            if now >= next_report:
                report(start)
                next_report += 1.0
            wake = min(next_monitor, next_report)
            if not args.listen_only:
                wake = min(wake, next_tx)
            time.sleep(max(0.0, wake - time.monotonic()))
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        for t in threads:
            t.join(timeout=1)
        for bus in buses.values():
            bus.shutdown()
        tp.close()


if __name__ == "__main__":
    main()
