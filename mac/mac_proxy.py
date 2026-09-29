#!/usr/bin/env python3
"""
mac_proxy.py -- macOS/Linux replacement for the Windows proxy_gui.py in
arjunr2/logitech-wheel-dev.

Reads a Logitech wheel through SDL (pygame) and sends the same UDP packets the
Windows proxy sends, so the unmodified proxy_receiver/receiver.c on the Pi
accepts them without knowing the difference.

Wire format, unchanged from the Windows proxy:

    [ 4-byte packet counter, little-endian ][ 272-byte DIJOYSTATE2 struct ]

DIJOYSTATE2 is a Microsoft DirectInput structure. We are not on Windows and
nothing here uses DirectInput -- we just rebuild the same 272 bytes by hand so
the C struct on the Pi still parses. Of those 272 bytes, the receiver only
looks at three fields:

    lX   -> steering
    lY   -> throttle
    lRz  -> brake

Everything else is zero-filled, exactly as it effectively was before.

Force feedback is not implemented. The return packet from the Pi is still read
and reported, so the link stays visible and the receiver needs no changes.

    pip install pygame-ce    # drop-in fork, still "import pygame"; plain
                             # pygame has no wheels for Python 3.14
    python3 mac_proxy.py --probe                 # find which axis is which
    python3 mac_proxy.py --remote 169.254.100.16 --steer 0 --throttle 1 --brake 2
"""

import argparse
import os
import socket
import struct
import sys
import time

# SDL wants a video subsystem to pump events through. On a headless box (or if
# you just don't want a window) this makes it use a stub driver instead.
os.environ.setdefault("SDL_VIDEODRIVER", "dummy")
os.environ.setdefault("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1")

import pygame  # noqa: E402  (must follow the env vars above)


# --- DIJOYSTATE2 -----------------------------------------------------------
# '<' means little-endian with no padding, which matches how the struct is laid
# out on both x86 and ARM when every member is a 4-byte int. Field order comes
# from proxy_receiver/state.h.
DIJOYSTATE2 = struct.Struct(
    "<"
    "6i"     # lX lY lZ lRx lRy lRz
    "2i"     # rglSlider[2]
    "4I"     # rgdwPOV[4]
    "128s"   # rgbButtons[128]
    "8i"     # lVX..lVRz, rglVSlider[2]     (velocity, unused)
    "8i"     # lAX..lARz, rglASlider[2]     (acceleration, unused)
    "8i"     # lFX..lFRz, rglFSlider[2]     (force, unused)
)
assert DIJOYSTATE2.size == 272, DIJOYSTATE2.size

POV_CENTERED = 0xFFFFFFFF   # DirectInput's "hat not pressed"
BUTTON_DOWN = 0x80          # DirectInput sets the high bit of the byte

# Axis full-scale. The Logitech SDK reports signed 16-bit on Windows; if your
# receiver prints different magnitudes when run against the Windows proxy,
# change this to match rather than rescaling on the Pi.
AXIS_MAX = 32767

# Fault injection: while the fault button is held, lX and lY are sent as these
# raw values instead of the clamped wheel readings. Both sit outside the valid
# range [-32767, 32766] so the Pi's range check should reject them. They still
# fit in the 32-bit int fields of DIJOYSTATE2.
FAULT_STEER = -40000      # below -32767
FAULT_THROTTLE = 40000    # above  32766


def pack_state(steer, throttle, brake, buttons, raw_steer=None, raw_throttle=None):
    """steer in [-1, 1]; throttle and brake in [0, 1]; buttons a list of bools.

    raw_steer / raw_throttle, when given, are written straight into lX / lY
    with no scaling or clamping (used for out-of-range fault injection).

    Convention, which you should record in the team-defined values table:
      lX   = -AXIS_MAX full left   ..  +AXIS_MAX full right
      lY   = 0 released            ..  +AXIS_MAX fully pressed
      lRz  = 0 released            ..  +AXIS_MAX fully pressed
    """
    lX = int(max(-1.0, min(1.0, steer)) * AXIS_MAX)
    lY = int(max(0.0, min(1.0, throttle)) * AXIS_MAX)
    lRz = int(max(0.0, min(1.0, brake)) * AXIS_MAX)
    if raw_steer is not None:
        lX = int(raw_steer)
    if raw_throttle is not None:
        lY = int(raw_throttle)

    btn = bytearray(128)
    for i, pressed in enumerate(buttons[:128]):
        btn[i] = BUTTON_DOWN if pressed else 0

    return DIJOYSTATE2.pack(
        lX, lY, 0, 0, 0, lRz,          # lX lY lZ lRx lRy lRz
        0, 0,                          # sliders
        POV_CENTERED, POV_CENTERED, POV_CENTERED, POV_CENTERED,
        bytes(btn),
        *([0] * 24),                   # velocity, acceleration, force blocks
    )


# --- wheel -----------------------------------------------------------------

def open_wheel(index):
    pygame.init()
    pygame.joystick.init()
    count = pygame.joystick.get_count()
    if count == 0:
        sys.exit("No joystick found. Is the wheel plugged in and powered?")
    if index >= count:
        sys.exit("Requested device %d but only %d present." % (index, count))
    js = pygame.joystick.Joystick(index)
    js.init()
    print("Wheel: %s  (%d axes, %d buttons)"
          % (js.get_name(), js.get_numaxes(), js.get_numbuttons()))
    return js


def pedal(raw, inverted):
    """SDL reports every axis as -1..1. A pedal rests at one end and travels to
    the other, so map it into 0..1 and let a flag pick which end is 'released'."""
    v = (1.0 - raw) / 2.0 if not inverted else (raw + 1.0) / 2.0
    return max(0.0, min(1.0, v))


def probe(js):
    print("\nMove one control at a time. Note which axis number responds.")
    print("Ctrl-C when you have all three.\n")
    try:
        while True:
            pygame.event.pump()
            cols = ["a%d %+.3f" % (i, js.get_axis(i)) for i in range(js.get_numaxes())]
            pressed = [str(i) for i in range(js.get_numbuttons()) if js.get_button(i)]
            line = "  ".join(cols)
            if pressed:
                line += "   btn " + ",".join(pressed)
            print("\r" + line.ljust(100), end="", flush=True)
            time.sleep(0.03)
    except KeyboardInterrupt:
        print("\n")


# --- main ------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description="Wheel -> UDP proxy (macOS/Linux)")
    ap.add_argument("--remote", default="172.26.74.45",
                    help="Pi address to send wheel state to")
    ap.add_argument("--send-port", type=int, default=8765)
    ap.add_argument("--listen", default="172.26.2.109",
                    help="local interface to receive the force packet on")
    ap.add_argument("--recv-port", type=int, default=8001)
    ap.add_argument("--interval-ms", type=int, default=20,
                    help="transmit period; R4.4 wants no slower than 50ms")
    ap.add_argument("--device", type=int, default=0)
    ap.add_argument("--steer", type=int, default=0, help="axis index")
    ap.add_argument("--throttle", type=int, default=1, help="axis index")
    ap.add_argument("--brake", type=int, default=2, help="axis index")
    ap.add_argument("--invert-throttle", action="store_true")
    ap.add_argument("--invert-brake", action="store_true")
    ap.add_argument("--fault-button", type=int, default=1,
                    help="button index (as shown by --probe) that injects "
                         "out-of-range steer/throttle while held; -1 disables")
    ap.add_argument("--fault-steer", type=int, default=FAULT_STEER,
                    help="raw lX sent while fault button held (default %(default)s)")
    ap.add_argument("--fault-throttle", type=int, default=FAULT_THROTTLE,
                    help="raw lY sent while fault button held (default %(default)s)")
    ap.add_argument("--probe", action="store_true",
                    help="print live axis values and exit")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    js = open_wheel(args.device)
    if args.fault_button >= js.get_numbuttons():
        sys.exit("--fault-button %d is out of range; wheel has %d buttons."
                 % (args.fault_button, js.get_numbuttons()))

    if args.probe:
        probe(js)
        return

    for name, idx in (("steer", args.steer), ("throttle", args.throttle),
                      ("brake", args.brake)):
        if idx >= js.get_numaxes():
            sys.exit("--%s %d is out of range; run --probe first." % (name, idx))

    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx.bind((args.listen, args.recv_port))
    rx.setblocking(False)

    period = args.interval_ms / 1000.0
    counter = 0
    last_force = None
    next_tx = time.monotonic()

    print("Sending to %s:%d every %dms, listening on %s:%d. Ctrl-C to stop.\n"
          % (args.remote, args.send_port, args.interval_ms,
             args.listen, args.recv_port))

    try:
        while True:
            pygame.event.pump()

            steer = js.get_axis(args.steer)
            throttle = pedal(js.get_axis(args.throttle), args.invert_throttle)
            brake = pedal(js.get_axis(args.brake), args.invert_brake)
            buttons = [js.get_button(i) for i in range(js.get_numbuttons())]
            inject = (0 <= args.fault_button < len(buttons)
                      and bool(buttons[args.fault_button]))

            now = time.monotonic()
            if now >= next_tx:
                next_tx += period
                if inject:
                    state = pack_state(steer, throttle, brake, buttons,
                                       raw_steer=args.fault_steer,
                                       raw_throttle=args.fault_throttle)
                else:
                    state = pack_state(steer, throttle, brake, buttons)
                packet = counter.to_bytes(4, "little") + state
                try:
                    tx.sendto(packet, (args.remote, args.send_port))
                except OSError as e:
                    if not args.quiet:
                        print("\nsend failed: %s" % e)
                counter += 1

            # Drain the return path. Nothing drives the wheel with it, but a
            # silent link is worth noticing, so we surface the last value.
            try:
                while True:
                    data, _ = rx.recvfrom(16)
                    if data:
                        last_force = int.from_bytes(data[:1], "little", signed=True)
            except BlockingIOError:
                pass
            except OSError:
                pass

            if not args.quiet:
                print("\rsteer %+.3f  thr %.2f  brk %.2f  sent %6d  force %s  %s"
                      % (steer, throttle, brake, counter,
                         "--" if last_force is None else "%+4d" % last_force,
                         "INJECT lX=%d lY=%d" % (args.fault_steer, args.fault_throttle)
                         if inject else " " * 24),
                      end="", flush=True)

            time.sleep(0.002)
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        tx.close()
        rx.close()
        pygame.quit()


if __name__ == "__main__":
    main()
