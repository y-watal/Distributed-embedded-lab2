#!/usr/bin/env python3
"""
send_self_test.py -- send a few valid packets, press the self-test button
(button 10 = 128) once, then quit.

Sequence (one packet every --interval-ms, default 50 ms):

  1. --count neutral packets (default 5): steering 0, pedals 0, no buttons.
     The first valid frame moves the STM32 from ERROR to NORMAL, and these
     frames make sure it has seen button 10 released.
  2. --press-packets packets with button 10 = 128 (default 1). blinkers.c
     sees the 0 -> 128 edge and, in NORMAL, requests SELF_TEST.
  3. Exit. Nothing else is sent, so ~90 ms later the STM32 reports
     LINK_TIMEOUT and goes to ERROR.

Don't run mac_proxy.py or the other send_*.py scripts at the same time.

    python3 send_self_test.py
    python3 send_self_test.py --count 10 --interval-ms 100
"""

import argparse
import socket
import sys
import time

from send_steer import pack_packet

SELF_TEST_BUTTON = 10


def main():
    ap = argparse.ArgumentParser(
        description="Send a few valid packets, press button 10 once, quit.")
    ap.add_argument("--count", type=int, default=5,
                    help="neutral packets before the press (default 5)")
    ap.add_argument("--press-packets", type=int, default=1,
                    help="packets with button 10 = 128 (default 1)")
    ap.add_argument("--remote", default="172.26.74.45",
                    help="Pi address (default matches mac_proxy.py)")
    ap.add_argument("--send-port", type=int, default=8765)
    ap.add_argument("--interval-ms", type=int, default=50,
                    help="packet period (default 50; keep >10 so the Pi "
                         "forwards every packet)")
    args = ap.parse_args()

    if args.count < 1 or args.press_packets < 1:
        sys.exit("--count and --press-packets must be at least 1.")
    if args.interval_ms <= 10:
        print("warning: the Pi drops packets <10 ms apart; "
              "some may not reach the STM32.", file=sys.stderr)

    plan = ([[]] * args.count) + ([[SELF_TEST_BUTTON]] * args.press_packets)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dest = (args.remote, args.send_port)
    period = args.interval_ms / 1000.0
    counter = int(time.time() * 1000) & 0xFFFFFFFF

    next_tx = time.monotonic()
    try:
        for i, buttons in enumerate(plan):
            delay = next_tx - time.monotonic()
            if delay > 0:
                time.sleep(delay)
            next_tx += period

            sock.sendto(pack_packet(counter + i, 0, 0, 0, buttons), dest)
            print("packet %2d  counter=0x%08X  button10=%s"
                  % (i + 1, (counter + i) & 0xFFFFFFFF,
                     "128" if buttons else "0"))
    except OSError as e:
        sys.exit("send failed: %s" % e)
    finally:
        sock.close()

    print("Done: %d valid + %d with button 10 pressed -> %s:%d. "
          "Link will time out now."
          % (args.count, args.press_packets, args.remote, args.send_port))


if __name__ == "__main__":
    main()
