#!/usr/bin/env python3
"""
send_steer.py -- hold a fixed wheel state over UDP, no wheel required.

Builds the same packet mac_proxy.py sends:

    [ 4-byte packet counter, little-endian ][ 272-byte DIJOYSTATE2 struct ]

so pi/uart_receiver.c forwards it to the STM32 exactly as if it came from
the wheel. Only lX (steering), lY (throttle), lRz (brake) and buttons
4/5/10 are forwarded; everything else is zero-filled like mac_proxy.py.

Values are RAW integers, i.e. exactly what the STM32 sees:
    steering   -32767 .. 32766   (steering.c saturates at +/-10000 = full lock)
    throttle        0 .. 32767
    brake           0 .. 32767
Anything outside those ranges is still sent (with a warning), so you can use
this to trigger BAD_RANGE on purpose.

The same packet is resent every --interval-ms (default 50 ms, inside
the STM32's 90 ms link timeout) until Ctrl-C, or for --hold-ms if given.
Don't run mac_proxy.py at the same time; its packets would overwrite these.

No dependencies beyond the standard library (does not need pygame).

    python3 send_steer.py 0                  # hold center until Ctrl-C
    python3 send_steer.py -10000             # hold full left
    python3 send_steer.py 5000 --hold-ms 2000   # hold for 2 s, then exit
    python3 send_steer.py --norm 0.5         # same scaling as mac_proxy.py
    python3 send_steer.py 40000              # deliberately out of range
"""

import argparse
import socket
import struct
import sys
import time

# Must match mac_proxy.py / proxy_receiver state.h. Duplicated rather than
# imported because importing mac_proxy pulls in pygame.
DIJOYSTATE2 = struct.Struct(
    "<"
    "6i"     # lX lY lZ lRx lRy lRz
    "2i"     # rglSlider[2]
    "4I"     # rgdwPOV[4]
    "128s"   # rgbButtons[128]
    "8i"     # velocity  (unused)
    "8i"     # acceleration (unused)
    "8i"     # force (unused)
)
assert DIJOYSTATE2.size == 272, DIJOYSTATE2.size

POV_CENTERED = 0xFFFFFFFF
BUTTON_DOWN = 0x80
AXIS_MAX = 32767            # same full-scale as mac_proxy.py

I32_MIN, I32_MAX = -2**31, 2**31 - 1

# Ranges accepted by src/tasks/communication.c command_in_range().
STEER_RANGE = (-32767, 32766)
PEDAL_RANGE = (0, 32767)


def pack_packet(counter, steer, throttle, brake, buttons):
    btn = bytearray(128)
    for i in buttons:
        btn[i] = BUTTON_DOWN
    state = DIJOYSTATE2.pack(
        steer, throttle, 0, 0, 0, brake,
        0, 0,
        POV_CENTERED, POV_CENTERED, POV_CENTERED, POV_CENTERED,
        bytes(btn),
        *([0] * 24),
    )
    return (counter & 0xFFFFFFFF).to_bytes(4, "little") + state


def check_range(name, value, lo, hi):
    if not (I32_MIN <= value <= I32_MAX):
        sys.exit("%s %d does not fit in an int32." % (name, value))
    if not (lo <= value <= hi):
        print("warning: %s %d is outside the STM32's accepted range %d..%d; "
              "expect BAD_RANGE." % (name, value, lo, hi), file=sys.stderr)


def main():
    ap = argparse.ArgumentParser(
        description="Hold a steering command over UDP until Ctrl-C.")
    ap.add_argument("steer",
                    help="raw lX value (int), or a float in [-1, 1] with --norm")
    ap.add_argument("--norm", action="store_true",
                    help="treat steer as -1..1 and scale by %d like mac_proxy.py"
                         % AXIS_MAX)
    ap.add_argument("--throttle", type=int, default=0, help="raw lY (default 0)")
    ap.add_argument("--brake", type=int, default=0, help="raw lRz (default 0)")
    ap.add_argument("--button", type=int, action="append", default=[],
                    metavar="N", help="press button N (0-127); repeatable")
    ap.add_argument("--remote", default="172.26.74.45",
                    help="Pi address (default matches mac_proxy.py)")
    ap.add_argument("--send-port", type=int, default=8765)
    ap.add_argument("--counter", type=lambda s: int(s, 0), default=None,
                    help="packet counter (default: derived from the clock)")
    ap.add_argument("--hold-ms", type=int, default=None,
                    help="stop after this long (default: hold until Ctrl-C)")
    ap.add_argument("--interval-ms", type=int, default=50,
                    help="resend period (default 50; keep under 90)")
    args = ap.parse_args()

    if args.norm:
        v = float(args.steer)
        steer = int(max(-1.0, min(1.0, v)) * AXIS_MAX)
    else:
        try:
            steer = int(args.steer, 0)
        except ValueError:
            sys.exit("steer must be an integer (use --norm for -1..1 floats).")

    check_range("steering", steer, *STEER_RANGE)
    check_range("throttle", args.throttle, *PEDAL_RANGE)
    check_range("brake", args.brake, *PEDAL_RANGE)
    for b in args.button:
        if not 0 <= b < 128:
            sys.exit("--button %d must be 0..127." % b)

    counter = (args.counter if args.counter is not None
               else int(time.time() * 1000) & 0xFFFFFFFF)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dest = (args.remote, args.send_port)
    sent = 0
    period = args.interval_ms / 1000.0
    start = time.monotonic()
    deadline = None if args.hold_ms is None else start + args.hold_ms / 1000.0
    next_tx = start

    print("Holding steer=%d thr=%d brk=%d buttons=%s -> %s:%d every %d ms. %s"
          % (steer, args.throttle, args.brake, args.button or "none",
             args.remote, args.send_port, args.interval_ms,
             "Ctrl-C to stop." if deadline is None
             else "Stopping after %d ms." % args.hold_ms))
    try:
        while deadline is None or time.monotonic() < deadline:
            pkt = pack_packet(counter + sent, steer, args.throttle,
                              args.brake, args.button)
            try:
                sock.sendto(pkt, dest)
                sent += 1
            except OSError as e:
                print("\nsend failed: %s" % e, file=sys.stderr)
            print("\rsent %6d" % sent, end="", flush=True)
            # Fixed-rate schedule so the period doesn't drift.
            next_tx += period
            delay = next_tx - time.monotonic()
            if delay > 0:
                time.sleep(delay)
    except KeyboardInterrupt:
        pass
    finally:
        sock.close()

    print("\nStopped after %d packet%s (%.1f s)."
          % (sent, "" if sent == 1 else "s", time.monotonic() - start))


if __name__ == "__main__":
    main()
