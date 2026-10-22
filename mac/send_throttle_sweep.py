#!/usr/bin/env python3
"""
send_throttle_sweep.py -- keep changing the throttle, one new value per packet.

Every packet carries a throttle that maps to a different CTRL throttle count
(raw >> 7), so every CTRL the cockpit forwards changes N3's target speed.
Each one reaches the motor, so N3's PWM_SET toggles once per packet. Steering 0.

Modes (one packet every --interval-ms, default 50 ms):

  ramp    triangle wave between --min and --max, --step raw per packet
          (default 1024 raw = 8 CTRL counts = ~20 mm/s per packet)
  step    jump between --min and --max every --every packets
          (default every packet: the cleanest edges for the scope). The
          --min level also presses the brake (--brake, default full), so
          each low step is a brake press: target -> 0, N3 brakes, and
          PWM_SET toggles. --no-brake steps the throttle only.
  random  a random level in [--min, --max] each packet, never the same
          CTRL count twice in a row

Ramp and random never press the brake.

On exit (Ctrl-C or --hold-ms) it sends a few zero-throttle packets so the
motors coast instead of waiting for the 90 ms CTRL timeout.

Mapping (cockpit -> N3): CTRL throttle t = raw >> 7 (0..255). t <= 2 is the
deadband (target 0); above it target = (t - 2) * 650 / 253 mm/s. A brake of
4 counts or more (raw >= 512) wins over the throttle: target 0, motors braked.

THE WHEELS WILL SPIN. Put the car on a stand or hold it.
Don't run mac_proxy.py or the other send_*.py scripts at the same time.

    python3 send_throttle_sweep.py                       # ramp, 1024..16384
    python3 send_throttle_sweep.py --mode step           # throttle 16384 <-> brake, every packet
    python3 send_throttle_sweep.py --mode step --no-brake    # throttle 16384 <-> 1024, no brake
    python3 send_throttle_sweep.py --mode step --every 10 --min 0 --max 32767
    python3 send_throttle_sweep.py --mode random --hold-ms 10000
"""

import argparse
import random
import socket
import sys
import time

from send_steer import pack_packet, check_range, PEDAL_RANGE

CTRL_SHIFT = 7          # cockpit: CTRL throttle = raw >> 7
DEADBAND = 2            # N3 drive.c CTRL_THROTTLE_DEADBAND
MAX_SPEED_MM_S = 650.0  # N3 drive.c MAX_TARGET_SPEED_MM_S
BRAKE_ACTIVE = 4        # N3 drive.c CTRL_BRAKE_ACTIVE (CTRL counts)


def ctrl_count(raw):
    return max(0, min(32767, raw)) >> CTRL_SHIFT


def target_mm_s(raw, brake=0):
    if ctrl_count(brake) >= BRAKE_ACTIVE:
        return 0.0
    t = ctrl_count(raw)
    return 0.0 if t <= DEADBAND else (t - DEADBAND) * MAX_SPEED_MM_S / (255 - DEADBAND)


def levels(args):
    """Yields one (raw throttle, raw brake) per packet, forever."""
    lo, hi = args.min, args.max
    if args.mode == "ramp":
        value, direction = lo, 1
        while True:
            yield value, 0
            nxt = value + direction * args.step
            if nxt > hi or nxt < lo:
                direction = -direction
                nxt = value + direction * args.step
            value = max(lo, min(hi, nxt))
    elif args.mode == "step":
        high = False
        while True:
            for _ in range(args.every):
                yield (hi, 0) if high else (lo, args.brake)
            high = not high
    else:  # random
        last = None
        while True:
            value = random.randint(lo, hi)
            if ctrl_count(value) != last:
                last = ctrl_count(value)
                yield value, 0


def main():
    ap = argparse.ArgumentParser(description="Keep changing the throttle over UDP.")
    ap.add_argument("--mode", choices=("ramp", "step", "random"), default="ramp")
    ap.add_argument("--min", type=int, default=1024, help="lowest raw throttle (default 1024)")
    ap.add_argument("--max", type=int, default=16384,
                    help="highest raw throttle (default 16384, about half speed)")
    ap.add_argument("--step", type=int, default=1024,
                    help="ramp: raw change per packet (default 1024; >= 128 so every "
                         "packet changes the CTRL count)")
    ap.add_argument("--every", type=int, default=1,
                    help="step: packets per level (default 1 = change every packet)")
    ap.add_argument("--brake", type=int, default=32767,
                    help="step: raw brake sent with the --min level (default 32767, full)")
    ap.add_argument("--no-brake", action="store_true",
                    help="step: don't press the brake at --min, just step the throttle")
    ap.add_argument("--remote", default="172.26.74.45",
                    help="Pi address (default matches mac_proxy.py)")
    ap.add_argument("--send-port", type=int, default=8765)
    ap.add_argument("--interval-ms", type=int, default=50,
                    help="packet period (default 50; keep under 90, the CTRL timeout)")
    ap.add_argument("--hold-ms", type=int, default=None,
                    help="stop after this long (default: until Ctrl-C)")
    ap.add_argument("--quiet", action="store_true", help="don't print every packet")
    args = ap.parse_args()

    check_range("--min", args.min, *PEDAL_RANGE)
    check_range("--max", args.max, *PEDAL_RANGE)
    check_range("--brake", args.brake, *PEDAL_RANGE)
    if args.mode != "step" or args.no_brake:
        args.brake = 0
    elif ctrl_count(args.brake) < BRAKE_ACTIVE:
        print("warning: --brake %d is below N3's brake threshold (512 raw); the low step "
              "won't brake." % args.brake, file=sys.stderr)
    if args.min >= args.max:
        sys.exit("--min must be below --max.")
    if args.mode == "ramp" and args.step < (1 << CTRL_SHIFT):
        print("warning: --step %d < 128 raw: some packets won't change the CTRL count, "
              "so they won't change the target." % args.step, file=sys.stderr)
    if args.mode == "ramp" and ctrl_count(args.min) <= DEADBAND:
        print("warning: --min %d is in N3's throttle deadband (target 0); packets near the "
              "bottom of the ramp won't change the target." % args.min, file=sys.stderr)
    if args.every < 1:
        sys.exit("--every must be at least 1.")
    if args.interval_ms >= 90:
        print("warning: --interval-ms %d >= 90 ms: the nodes will time out between "
              "packets (CTRL_TIMEOUT)." % args.interval_ms, file=sys.stderr)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dest = (args.remote, args.send_port)
    counter = int(time.time() * 1000) & 0xFFFFFFFF
    period = args.interval_ms / 1000.0
    start = time.monotonic()
    deadline = None if args.hold_ms is None else start + args.hold_ms / 1000.0
    next_tx = start
    sent = changes = 0
    prev_target = None

    print("%s throttle %d..%d%s -> %s:%d every %d ms. THE WHEELS WILL SPIN. %s"
          % (args.mode, args.min, args.max,
             " (brake %d at the low step)" % args.brake if args.brake else "",
             args.remote, args.send_port, args.interval_ms,
             "Ctrl-C to stop." if deadline is None else "Stopping after %d ms." % args.hold_ms))
    try:
        for raw, brake in levels(args):
            if deadline is not None and time.monotonic() >= deadline:
                break
            try:
                sock.sendto(pack_packet(counter + sent, 0, raw, brake, []), dest)
            except OSError as e:
                print("\nsend failed: %s" % e, file=sys.stderr)
            else:
                tgt = target_mm_s(raw, brake)
                if prev_target is not None and tgt != prev_target:
                    changes += 1
                prev_target = tgt
                sent += 1
                if not args.quiet:
                    print("\r#%6d  thr %5d (CTRL %3d)  brake %-5s  target %5.1f mm/s  "
                          "(target changes %d)"
                          % (sent, raw, ctrl_count(raw), "ON" if brake else "off", tgt, changes),
                          end="", flush=True)
            next_tx += period  # fixed-rate schedule, no drift
            delay = next_tx - time.monotonic()
            if delay > 0:
                time.sleep(delay)
    except KeyboardInterrupt:
        pass
    finally:
        # Leave the car coasting rather than relying on the CTRL timeout.
        for i in range(3):
            try:
                sock.sendto(pack_packet(counter + sent + i, 0, 0, 0, []), dest)
            except OSError:
                pass
            time.sleep(period)
        sock.close()

    print("\nStopped after %d packets (%.1f s), %d target changes; sent throttle 0 to finish."
          % (sent, time.monotonic() - start, changes))


if __name__ == "__main__":
    main()
