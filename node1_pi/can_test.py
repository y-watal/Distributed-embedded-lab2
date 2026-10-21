#!/usr/bin/env python3
"""Lab 3 CAN bring-up test, node N1 (Pi).

Sends HB_N1 (0x100) on can0 (bus A) and can1 (bus B) every 100 ms with one
shared 4-bit sequence counter in the low nibble of byte 0, counts every
received frame per bus and per ID, and prints a summary once a second:

    [N1 12.0s]
      A can0: ERROR-ACTIVE | tx ok=10 err=0 | rx: 0x101 x10 0x102 x10
      B can1: ERROR-ACTIVE | tx ok=10 err=0 | rx: 0x101 x10 0x102 x10

"gaps" after an ID counts sequence numbers that were skipped (lost frames).
Run ./can_up.sh first. Needs python-can: sudo apt install python3-can
"""
import argparse
import json
import subprocess
import threading
import time

import can

HB_ID = 0x100  # HB_N1 in the message catalog
BUSES = {"A": "can0", "B": "can1"}

lock = threading.Lock()
rx_stats = {name: {} for name in BUSES}  # id -> [count, gaps, last_seq]
tx_stats = {name: [0, 0] for name in BUSES}  # [ok, err]


def rx_loop(name, bus, stop):
    while not stop.is_set():
        msg = bus.recv(timeout=0.2)
        if msg is None or msg.is_error_frame:
            continue
        seq = msg.data[0] & 0x0F if msg.dlc else None
        with lock:
            s = rx_stats[name].setdefault(msg.arbitration_id, [0, 0, None])
            s[0] += 1
            if seq is not None:
                if s[2] is not None and seq != (s[2] + 1) & 0x0F:
                    s[1] += 1
                s[2] = seq


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


def report(start):
    print(f"[N1 {time.monotonic() - start:.1f}s]")
    with lock:
        for name, channel in BUSES.items():
            ok, err = tx_stats[name]
            tx_stats[name] = [0, 0]
            ids = rx_stats[name]
            if ids:
                parts = []
                for can_id in sorted(ids):
                    s = ids[can_id]
                    part = f"0x{can_id:03x} x{s[0]}"
                    if s[1]:
                        part += f" (gaps {s[1]})"
                    parts.append(part)
                    s[0] = s[1] = 0
                rx = " ".join(parts)
            else:
                rx = "(nothing yet)"
            print(f"  {name} {channel}: {link_state(channel)} | tx ok={ok} err={err} | rx: {rx}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--period-ms", type=int, default=100, help="heartbeat period (default 100)")
    ap.add_argument("--listen-only", action="store_true", help="receive only, send nothing")
    args = ap.parse_args()

    buses = {}
    for name, channel in BUSES.items():
        try:
            buses[name] = can.Bus(channel=channel, interface="socketcan")
        except (OSError, can.CanError) as e:
            print(f"bus {name} ({channel}) not available: {e}")
    if not buses:
        raise SystemExit("no CAN interface is up - run ./can_up.sh")

    stop = threading.Event()
    threads = [threading.Thread(target=rx_loop, args=(n, b, stop), daemon=True)
               for n, b in buses.items()]
    for t in threads:
        t.start()

    period = args.period_ms / 1000.0
    start = time.monotonic()
    next_tx = start
    next_report = start + 1.0
    seq = 0
    health = (0x01 if "A" in buses else 0) | (0x02 if "B" in buses else 0)

    try:
        while True:
            now = time.monotonic()
            if not args.listen_only and now >= next_tx:
                # byte 0: low nibble seq, high nibble flags; byte 1: zone state; byte 2: bus health
                msg = can.Message(arbitration_id=HB_ID, is_extended_id=False,
                                  data=[seq, 0, health, 0, 0, 0, 0, 0])
                for name, bus in buses.items():
                    try:
                        bus.send(msg, timeout=0)
                        ok = True
                    except can.CanError:
                        ok = False  # TX queue full (no ACK on the bus) or interface down
                    with lock:
                        tx_stats[name][0 if ok else 1] += 1
                seq = (seq + 1) & 0x0F
                next_tx += period
            if now >= next_report:
                report(start)
                next_report += 1.0
            wake = next_report if args.listen_only else min(next_tx, next_report)
            time.sleep(max(0.0, wake - time.monotonic()))
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        for t in threads:
            t.join(timeout=1)
        for bus in buses.values():
            bus.shutdown()


if __name__ == "__main__":
    main()
