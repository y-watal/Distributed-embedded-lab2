#!/usr/bin/env python3
"""
send_left_signal.py -- turn on the LEFT turn signal over UDP (button 5 = 128).

blinkers.c triggers on a rising edge of button 5 (0 -> 128), not on the level,
and pressing the active side again turns it off. So this script:

  1. sends button 5 released for --settle-ms (restores the link and makes
     sure the STM32 has seen the button up),
  2. sends button 5 = 128 for --press-ms (the "press"),
  3. releases it and keeps sending that state until Ctrl-C, so the link
     doesn't time out (a timeout drops the car to ERROR and hazards).

Steering is held at --steer (default 0). The firmware cancels the left signal
once steering has gone past -5000 and comes back above -4000, so a held
center value leaves it blinking.

If the left signal is already on, running this turns it OFF (toggle).
Don't run mac_proxy.py or send_steer.py at the same time.

    python3 send_left_signal.py
    python3 send_left_signal.py --steer -3000
"""

import argparse
import socket
import sys
import time

from send_steer import pack_packet, check_range, STEER_RANGE

LEFT_BUTTON = 5


def main():
    ap = argparse.ArgumentParser(description="Send the left turn signal over UDP.")
    ap.add_argument("--steer", type=int, default=0, help="raw lX to hold (default 0)")
    ap.add_argument("--remote", default="172.26.74.45",
                    help="Pi address (default matches mac_proxy.py)")
    ap.add_argument("--send-port", type=int, default=8765)
    ap.add_argument("--interval-ms", type=int, default=50,
                    help="resend period (default 50; keep under 90)")
    ap.add_argument("--settle-ms", type=int, default=200,
                    help="released time before the press (default 200)")
    ap.add_argument("--press-ms", type=int, default=100,
                    help="how long button 5 is held at 128 (default 100)")
    ap.add_argument("--hold-ms", type=int, default=None,
                    help="stop this long after the press "
                         "(default: hold until Ctrl-C)")
    args = ap.parse_args()

    check_range("steering", args.steer, *STEER_RANGE)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dest = (args.remote, args.send_port)
    period = args.interval_ms / 1000.0
    counter = int(time.time() * 1000) & 0xFFFFFFFF
    sent = 0

    start = time.monotonic()
    press_at = start + args.settle_ms / 1000.0
    release_at = press_at + args.press_ms / 1000.0
    stop_at = None if args.hold_ms is None else release_at + args.hold_ms / 1000.0

    print("Left signal -> %s:%d  (steer=%d, every %d ms). %s"
          % (args.remote, args.send_port, args.steer, args.interval_ms,
             "Ctrl-C to stop." if stop_at is None
             else "Stopping %d ms after the press." % args.hold_ms))

    next_tx = start
    try:
        while stop_at is None or time.monotonic() < stop_at:
            now = time.monotonic()
            pressed = press_at <= now < release_at
            if pressed:
                phase = "PRESSED "
            elif now < press_at:
                phase = "settling"
            else:
                phase = "released"
            pkt = pack_packet(counter + sent, args.steer, 0, 0,
                              [LEFT_BUTTON] if pressed else [])
            try:
                sock.sendto(pkt, dest)
                sent += 1
            except OSError as e:
                print("\nsend failed: %s" % e, file=sys.stderr)
            print("\rbutton5 %s  sent %6d" % (phase, sent), end="", flush=True)

            next_tx += period
            delay = next_tx - time.monotonic()
            if delay > 0:
                time.sleep(delay)
    except KeyboardInterrupt:
        pass
    finally:
        sock.close()

    print("\nStopped after %d packets (%.1f s)." % (sent, time.monotonic() - start))


if __name__ == "__main__":
    main()
