#!/usr/bin/env python3
"""Exercises ./cockpit without CAN hardware.

The two buses are socket pairs handed to the cockpit through
COCKPIT_TEST_FD_A/B. This script plays the bus, N2 (which relays) and N3:
it sends their heartbeats, FAULTs and FORCE, sends wheel packets like the
Mac proxy, and checks every frame the cockpit writes.

    make && python3 test_cockpit.py          # runs anywhere Linux, no root
"""
import os
import select
import signal
import socket
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import catalog as cat  # noqa: E402

CAN_FMT = "<IB3x8s"            # struct can_frame: id, dlc, pad, data[8]
JS = struct.Struct("<6i2i4I128s8i8i8i")
UDP_PORT, FORCE_PORT = 18765, 18001
results = []


def check(name, ok, detail=""):
    results.append((name, ok))
    print(("PASS " if ok else "FAIL ") + name + (f"  ({detail})" if detail else ""))


class Sim:
    def __init__(self, binary):
        self.bus = {}
        child = {}
        for b in "AB":
            mine, theirs = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
            mine.setblocking(False)
            self.bus[b], child[b] = mine, theirs
        env = dict(os.environ, COCKPIT_TEST_FD_A=str(child["A"].fileno()),
                   COCKPIT_TEST_FD_B=str(child["B"].fileno()))
        self.proc = subprocess.Popen(
            [binary, "--port", str(UDP_PORT), "--bind", "127.0.0.1",
             "--force-host", "127.0.0.1", "--force-port", str(FORCE_PORT)],
            env=env, pass_fds=[child["A"].fileno(), child["B"].fileno()],
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        for s in child.values():
            s.close()
        self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.force_rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.force_rx.bind(("127.0.0.1", FORCE_PORT))
        self.force_rx.setblocking(False)
        self.seq = {}
        self.frames = []          # (t, bus, id, data) written by the cockpit
        self.t0 = time.monotonic()
        self.nodes_on = {"A": {1, 2}, "B": {1, 2}}   # which simulated nodes are on each bus
        self.relay = True
        self.n3_fault = False
        self.n3_state = 2
        self.wheel = None         # (steer, throttle, brake, buttons) or None = proxy stopped
        self.next_hb = self.next_udp = self.next_force = time.monotonic()
        self.counter = 0
        self.forces = []

    def send(self, bus, can_id, data):
        try:
            self.bus[bus].send(struct.pack(CAN_FMT, can_id, 8, bytes(data)))
        except (BlockingIOError, OSError):
            pass

    def emit(self, node, can_id, payload):
        s = self.seq.get(can_id, 0)
        self.seq[can_id] = (s + 1) & 0x0F
        data = bytes([s]) + bytes(payload[1:8])
        on = [b for b in "AB" if node in self.nodes_on[b]]
        for b in on:
            self.send(b, can_id, data)
        # N2 forwards N3's frames to a bus N3 isn't on, if N2 is on it
        if self.relay and node == 2:
            for b in "AB":
                if b not in on and 1 in self.nodes_on[b] and any(o in self.nodes_on[o2] for o2 in on for o in [1]):
                    self.send(b, can_id, data)

    def step(self, until):
        while time.monotonic() < until:
            now = time.monotonic()
            if now >= self.next_hb:
                self.next_hb += 0.020
                self.emit(1, 0x101, [0, 2, 0x10, 0, 0, 0, 0, 0])
                self.emit(2, 0x102, [0, self.n3_state | (0x10 if self.n3_fault else 0), 0, 1 if self.n3_fault else 0, 0, 0, 0, 0])
            if now >= self.next_force:
                self.next_force += 0.050
                self.emit(1, 0x300, [0, (self.counter * 3) & 0x7F, 0, 0, 0, 0, 0, 0])
            if self.wheel and now >= self.next_udp:
                self.next_udp += 0.020
                steer, thr, brk, buttons = self.wheel
                btn = bytearray(128)
                for i in buttons:
                    btn[i] = 0x80
                pkt = self.counter.to_bytes(4, "little") + JS.pack(
                    steer, thr, 0, 0, 0, brk, 0, 0, *([0xFFFFFFFF] * 4), bytes(btn), *([0] * 24))
                self.counter += 1
                self.udp.sendto(pkt, ("127.0.0.1", UDP_PORT))
            elif not self.wheel:
                self.next_udp = now
            r, _, _ = select.select([self.bus["A"], self.bus["B"], self.force_rx], [], [], 0.001)
            for s in r:
                if s is self.force_rx:
                    self.forces.append(s.recv(16))
                    continue
                b = "A" if s is self.bus["A"] else "B"
                while True:
                    try:
                        raw = s.recv(16)
                    except BlockingIOError:
                        break
                    cid, dlc, data = struct.unpack(CAN_FMT, raw)
                    self.frames.append((time.monotonic(), b, cid, data))

    def run(self, seconds):
        self.step(time.monotonic() + seconds)

    def since(self, t, can_id=None, bus=None):
        return [f for f in self.frames if f[0] >= t and (can_id is None or f[2] == can_id)
                and (bus is None or f[1] == bus)]


def hb_state(frames):
    return [(f[3][1] & 0x0F, bool(f[3][1] & 0x10), f[3][3]) for f in frames]


def main():
    binary = os.path.join(os.path.dirname(os.path.abspath(__file__)), "cockpit")
    sim = Sim(binary)
    try:
        # 1. Power-up, proxy stopped: both zones' heartbeats, no wheel data.
        t = time.monotonic(); sim.run(0.5)
        hbs = sim.since(t + 0.1, 0x100, "A")
        st = hb_state(hbs)
        check("HB_N1 every 20 ms on both buses", 20 <= len(hbs) <= 27 and len(sim.since(t + 0.1, 0x100, "B")) >= 20,
              f"{len(hbs)} on A in 0.4 s")
        check("power-up: BOOT with local fault (no wheel data)", st and st[-1][0] == 0 and st[-1][1], str(st[-1:]))
        bl = sim.since(t, 0x200, "A")
        check("power-up: BLINK is hazard", bl and (bl[-1][3][1] & 3) == 3, f"last BLINK byte1 0x{bl[-1][3][1]:02x}" if bl else "none")
        check("no CTRL without wheel data", not sim.since(t, 0x010))

        # 2. Proxy starts: CTRL on every packet, state NORMAL.
        sim.wheel = (1000, 16384, 0, [])
        t = time.monotonic(); sim.run(0.6)
        ca, cb = sim.since(t, 0x010, "A"), sim.since(t, 0x010, "B")
        check("CTRL on both buses for every wheel packet", abs(len(ca) - 30) <= 3 and abs(len(cb) - 30) <= 3,
              f"A {len(ca)}, B {len(cb)} in 0.6 s")
        d = ca[-1][3]
        expect = cat.ctrl_payload(d[0] & 0x0F, *cat.ctrl_from_lab2(1000, 16384, 0, 0, 0, 0))
        check("CTRL bytes and CRC match catalog.py", d == expect, d.hex())
        st = hb_state(sim.since(t + 0.3, 0x100, "A"))
        check("NORMAL once both zones and the wheel are present", st and st[-1][:2] == (2, False), str(st[-1:]))
        bl = sim.since(t + 0.3, 0x200, "A")
        check("BLINK off when NORMAL", bl and (bl[-1][3][1] & 3) == 0)
        seqs = [f[3][0] & 0x0F for f in ca]
        check("CTRL sequence counts up by one", all((b - a) & 0x0F == 1 for a, b in zip(seqs, seqs[1:])))

        # 3. Left signal: button 5 tap -> BLINK LEFT, phase toggling every 500 ms.
        sim.wheel = (1000, 16384, 0, [5]); sim.run(0.06)
        sim.wheel = (1000, 16384, 0, []); t = time.monotonic(); sim.run(1.2)
        bl = [f for f in sim.since(t - 0.07, 0x200, "A")]
        left = [f for f in bl if (f[3][1] & 3) == 1]
        phases = [bool(f[3][1] & 4) for f in left]
        gaps = [round(b[0] - a[0], 3) for a, b in zip(left, left[1:])]
        check("button 5 -> BLINK LEFT, phase alternating", len(left) >= 3 and all(p != q for p, q in zip(phases, phases[1:])),
              f"phases {phases}")
        check("BLINK toggles every 500 ms", gaps and all(0.47 <= g <= 0.53 for g in gaps), str(gaps))

        # 4. Steering past +-5000 then back cancels the left signal.
        sim.wheel = (-8000, 16384, 0, []); sim.run(0.2)
        sim.wheel = (-1000, 16384, 0, []); t = time.monotonic(); sim.run(0.6)
        bl = sim.since(t, 0x200, "A")
        check("left signal cancels after the turn", bl and (bl[-1][3][1] & 3) == 0)

        # 5. Fault injection: SIGUSR2 = out-of-range steering, SIGUSR1 = bad CRC.
        t = time.monotonic(); sim.proc.send_signal(signal.SIGUSR2); sim.run(0.1)
        c = sim.since(t, 0x010, "A")
        bad = [f for f in c if struct.unpack("<h", f[3][1:3])[0] == -32768]
        check("SIGUSR2 -> exactly one CTRL with steer -32768", len(bad) == 1)
        t = time.monotonic(); sim.proc.send_signal(signal.SIGUSR1); sim.run(0.1)
        c = sim.since(t, 0x010, "A")
        crc_bad = [f for f in c if cat.crc8_j1850(f[3][:6]) != f[3][6]]
        check("SIGUSR1 -> exactly one CTRL with a bad CRC", len(crc_bad) == 1)
        sim.wheel = (-40000, 40000, 0, []); t = time.monotonic(); sim.run(0.1)
        c = sim.since(t, 0x010, "A")
        check("out-of-range wheel value -> steer -32768, not wrapped",
              c and all(struct.unpack("<h", f[3][1:3])[0] == -32768 for f in c[1:]))
        sim.wheel = (0, 0, 0, [])

        # 6. N3 reports a local fault, then clears it.
        sim.n3_fault = True; sim.emit(2, 0x022, [0, 1, 0, 0, 0, 0, 0, 0])
        t = time.monotonic(); sim.run(0.1)
        st = hb_state(sim.since(t, 0x100, "A"))
        bl = sim.since(t, 0x200, "A")
        check("FAULT_N3 -> cockpit SYS_ERROR, hazards", st and st[-1][0] == 1 and bl and (bl[-1][3][1] & 3) == 3,
              f"state {st[-1:]}")
        sim.n3_fault = False; t = time.monotonic(); sim.run(0.15)
        st = hb_state(sim.since(t + 0.05, 0x100, "A"))
        check("N3 fault cleared -> NORMAL again", st and st[-1][0] == 2, str(st[-1:]))

        # 6b. N3 reboots (heartbeat says BOOT): the cockpit stays in error until N3 is ready.
        sim.n3_state = 0; t = time.monotonic(); sim.run(0.1)
        st = hb_state(sim.since(t + 0.03, 0x100, "A"))
        check("N3 heartbeat in BOOT -> cockpit SYS_ERROR", st and st[-1][0] == 1, str(st[-1:]))
        sim.n3_state = 2; t = time.monotonic(); sim.run(0.1)
        st = hb_state(sim.since(t + 0.03, 0x100, "A"))
        check("N3 ready again -> NORMAL", st and st[-1][0] == 2, str(st[-1:]))

        # 7. FORCE from N2 is forwarded to the proxy over UDP.
        n = len(sim.forces); sim.run(0.3)
        check("FORCE forwarded to the proxy", len(sim.forces) - n >= 4, f"{len(sim.forces) - n} in 0.3 s")

        # 8. Scenario (c): N1 off bus B -> nothing from N2/N3 reaches the cockpit on B.
        sim.nodes_on["B"] = set(); sim.relay = False
        t = time.monotonic(); sim.run(0.4)
        hbs = sim.since(t + 0.1, 0x100, "A")
        st = hb_state(hbs)
        ctrl_b = sim.since(t + 0.1, 0x010, "B")
        check("bus B degraded: HB_N1 reports B_DEGRADED", hbs and (hbs[-1][3][2] & 2), f"byte2 0x{hbs[-1][3][2]:02x}")
        check("bus B degraded: no CTRL on B, heartbeats still on B", not ctrl_b and sim.since(t + 0.1, 0x100, "B"))
        check("bus B degraded: still NORMAL (peers heard on A)", st and st[-1][0] == 2)
        sim.nodes_on["B"] = {1, 2}; sim.relay = True; sim.run(0.2)

        # 9. Wheel button 10: single press = test fault, double press = recover.
        sim.wheel = (0, 0, 0, [10]); sim.run(0.06); sim.wheel = (0, 0, 0, [])
        t = time.monotonic(); sim.run(0.1)
        f1 = sim.since(t - 0.06, 0x020, "A")
        st = hb_state(sim.since(t, 0x100, "A"))
        check("button 10 -> FAULT_N1 TEST_BUTTON, SYS_ERROR with local fault",
              any(f[3][1] == 1 for f in f1) and st and st[-1] == (1, True, 1), str(st[-1:]))
        for _ in range(2):
            sim.wheel = (0, 0, 0, [10]); sim.run(0.06); sim.wheel = (0, 0, 0, []); sim.run(0.06)
        t = time.monotonic(); sim.run(0.1)
        st = hb_state(sim.since(t, 0x100, "A"))
        check("double press -> recovered, byte 3 keeps the last fault",
              st and st[-1] == (2, False, 1), str(st[-1:]))

        # 10. Proxy stops: UDP timeout after 90 ms -> FAULT_N1 UDP_TIMEOUT.
        sim.wheel = None; t = time.monotonic(); sim.run(0.3)
        f2 = [f for f in sim.since(t, 0x020, "A") if f[3][1] == 2]
        first = sim.since(t, 0x100, "A")
        lf = [f for f in first if f[3][1] & 0x10]
        check("wheel data stops -> FAULT_N1 UDP_TIMEOUT within ~100 ms",
              f2 and 0.08 <= f2[0][0] - t <= 0.13, f"after {round(f2[0][0] - t, 3)} s" if f2 else "none")
        check("... and HB_N1 carries LOCAL_FAULT", bool(lf))
    finally:
        sim.proc.send_signal(signal.SIGINT)
        try:
            out, _ = sim.proc.communicate(timeout=3)
        except subprocess.TimeoutExpired:
            sim.proc.kill()
            out, _ = sim.proc.communicate()
    lat = [l for l in out.splitlines() if "latency" in l]
    print("\ncockpit output (last summary):")
    blocks = out.split("[N1 ")
    print("[N1 " + blocks[-1].strip() if len(blocks) > 1 else out[-1500:])
    if lat:
        print("latency lines seen:", lat[-1].strip())
    failed = [n for n, ok in results if not ok]
    print(f"\n{len(results) - len(failed)}/{len(results)} checks passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
