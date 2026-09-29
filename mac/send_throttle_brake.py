#!/usr/bin/env python3
"""
send_throttle_brake.py -- send throttle packets, then brake packets with the
throttle still held, then quit.

Sequence (one packet every --interval-ms, default 50 ms; steering 0):

  1. THROTTLE  --throttle-packets packets: throttle=--throttle, brake=0
  2. BRAKE     --brake-packets packets:    throttle=--throttle, brake=--brake
  3. Exit. The STM32 reports LINK_TIMEOUT ~90 ms later and holds the brake.

In motor_control.c the brake wins over throttle once brake >= 500
(BRAKE_ACTIVE_THRESHOLD), so phase 2 should stop the wheels even with the
throttle still on. Throttle below 300 is inside the deadband and does nothing;
32767 asks for 650 mm/s.

THE WHEELS WILL SPIN. Put the car on a stand or hold it.
Don't run mac_proxy.py or the other send_*.py scripts at the same time.

    python3 send_throttle_brake.py
    python3 send_throttle_brake.py --throttle 32767 --throttle-packets 100 --brake-packets 50
"""

import argparse
import socket
import sys
import time

from send_steer import pack_packet, check_range, PEDAL_RANGE


def main():
    ap = argparse.ArgumentParser(
        description="Send throttle, then brake with throttle held, then quit.")
    ap.add_argument("--throttle", type=int, default=16384,
                    help="raw lY for both phases (default 16384, ~half)")
    ap.add_argument("--brake", type=int, default=32767,
                    help="raw lRz for the brake phase (default 32767)")
    ap.add_argument("--throttle-packets", type=int, default=75,
                    help="number of throttle-only packets (default 75)")
    ap.add_argument("--brake-packets", type=int, default=50,
                    help="number of brake packets (default 50)")
    ap.add_argument("--remote", default="172.26.74.45",
                    help="Pi address (default matches mac_proxy.py)")
    ap.add_argument("--send-port", type=int, default=8765)
    ap.add_argument("--interval-ms", type=int, default=50,
                    help="packet period (default 50; keep between 10 and 90)")
    args = ap.parse_args()

    check_range("throttle", args.throttle, *PEDAL_RANGE)
    check_range("brake", args.brake, *PEDAL_RANGE)
    if args.brake < 500:
        print("warning: brake %d is below the firmware's 500 threshold; "
              "it will not override throttle." % args.brake, file=sys.stderr)

    period = args.interval_ms / 1000.0
    n_throttle = args.throttle_packets
    n_brake = args.brake_packets
    if n_throttle < 0 or n_brake < 0:
        sys.exit("packet counts can't be negative.")
    plan = ([("THROTTLE", args.throttle, 0)] * n_throttle
            + [("BRAKE   ", args.throttle, args.brake)] * n_brake)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dest = (args.remote, args.send_port)
    counter = int(time.time() * 1000) & 0xFFFFFFFF

    print("-> %s:%d every %d ms: %d throttle packets, then %d brake packets."
          % (args.remote, args.send_port, args.interval_ms, n_throttle, n_brake))

    sent = 0
    next_tx = time.monotonic()
    try:
        for phase, thr, brk in plan:
            delay = next_tx - time.monotonic()
            if delay > 0:
                time.sleep(delay)
            next_tx += period

            sock.sendto(pack_packet(counter + sent, 0, thr, brk, []), dest)
            sent += 1
            print("\r%s  thr=%5d  brk=%5d  sent %4d/%d"
                  % (phase, thr, brk, sent, len(plan)), end="", flush=True)
    except OSError as e:
        sys.exit("\nsend failed: %s" % e)
    except KeyboardInterrupt:
        print("\nInterrupted.", end="")
    finally:
        sock.close()

    print("\nDone after %d packets. Link will time out now (motors braked)." % sent)


if __name__ == "__main__":
    main()
