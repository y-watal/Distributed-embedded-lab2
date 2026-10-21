#!/usr/bin/env python3
"""
Lab 3 Part 3: real-time analysis on ONE bus (relay off), three layers:

  3.1 node response times, N2 and N3 (Zephyr, fixed-priority preemptive)
  3.2 CAN message response times (Davis, Burns, Bril, Lukkien 2007)
  3.3 end-to-end chains

Every number lives in the INPUTS section with where it came from. Most WCETs
are still assumed (handout placeholders or an SPI byte-count estimate); when
you measure one, change it here, set its source to "measured: <how>", and
rerun:

    python3 docs/analysis/lab3_rta.py                 # both scenarios
    python3 docs/analysis/lab3_rta.py --pi-us 400     # with a measured Pi stage
    python3 docs/analysis/lab3_rta.py --scenario as_built

Two scenarios:
  as_built  the code at commit 9342b39
  proposed  three changes (docs/lab3_part3_analysis.md, "What has to change"):
            k_sched_lock() around the SPI part of canbus queue_on(),
            TEST_BTN below DRIVE / STEER / BLINK, and the Pi writing its own
            frames after CTRL in the UDP thread

All times in microseconds.
"""
import argparse
import math

# ======================================================================
# INPUTS
# ======================================================================

TAU = 2                 # bit time at 500 kbit/s
C_FRAME = 270           # DLC 8, worst-case stuffing: (47 + 64 + 24) * 2
FRAME_MIN = 222         # DLC 8, no stuffing, incl. 3-bit IFS: 111 bits * 2.
                        # Minimum spacing of frame-end events on one bus, so at
                        # most one RX or TX-done driver job per 222 us.

# SPI1 sits on APB2 = 84 MHz. Zephyr's STM32 SPI driver picks the first
# 84 MHz / 2^n that is <= spi-max-frequency, so the overlay's 4 MHz request
# runs at 84 / 32 = 2.625 MHz. ("proposed" asks for 10 MHz -> 84 / 16 = 5.25 MHz;
# 10.5 MHz would exceed the MCP2515's 10 MHz limit.) Polling mode
# (CONFIG_SPI_STM32_INTERRUPT off): the transfer runs in the caller's thread.
APB2_HZ = 84_000_000
SPI_REQUEST_HZ = {"as_built": 4_000_000, "proposed": 10_000_000}


def spi_hz(scen):
    n = 1
    while APB2_HZ >> n > SPI_REQUEST_HZ[scen]:
        n += 1
    return APB2_HZ >> n


# Cost of one spi_transceive: a + n * b.  ASSUMED - measure on the module's CS
# line (one transaction = one CS low pulse) and put the numbers here.
SPI_A = 8.0             # per transaction: k_sem lock, configure, CS GPIO, unlock
SPI_GAP = 0.5           # per byte, polling-loop gap on top of the wire time


class Costs:
    """Zephyr MCP2515 driver (drivers/can/can_mcp2515.c), per event:
      RX:      INT ISR, wake coop thread, READ CANINTF (3 B), READ RX BUFFER
               (1 + 13 B, clears RXnIF), filter loop, our rx_cb (CAN_RX_x
               toggle + k_msgq_put), read INT pin
      TX done: INT ISR, READ CANINTF (3 B), tx_done cb, BIT MODIFY CANINTF (4 B)
      send:    in the CALLER's thread: tx_sem, mutex, LOAD TX BUFFER (14 B), RTS (1 B)
    """

    def __init__(self, scen):
        self.hz = spi_hz(scen)
        self.byte = 8e6 / self.hz + SPI_GAP
        spi = lambda n: SPI_A + self.byte * n
        self.wire_rx = 17 * 8e6 / self.hz                       # wire time to read one frame
        self.drv_rx = round(6 + spi(3) + spi(14) + 6 + 1)
        self.drv_txdone = round(6 + spi(3) + 4 + spi(4) + 1)
        self.send = round(4 + spi(14) + spi(1) + 1)             # sched-locked in "proposed"
        self.txn_max = round(spi(14))                            # longest single transaction


B_IRQ = 10              # longest IRQ-off section (k_spin_lock in zone_update), ASSUMED
B_DRIVE_LOCK = 25       # PID's longest drive_lock section (duty write / brake), ASSUMED

# Pi (N1), all ASSUMED until measured
PI_TIMER_JITTER = 1500  # timer-thread wakeup latency (HB_N1, BLINK, FAULT_N1, HB_MON)
DELTA_PI = 150          # mcp251x: write() or TX-done IRQ -> frame loaded + RTS in TXB0

PWM_MOTOR_PERIOD = 100  # 10 kHz, preload on: new duty reaches PWM_OUT within one period
PWM_SERVO_PERIOD = 20000  # 50 Hz servo, preload on

# Zone timing (lab3_config.h)
HB_PERIOD = 20000
MONITOR_PERIOD = 5000
ACTUATOR_POLL = 5000
BLINK_POLL = 10000
FAIL_TIMEOUT = 66000
FAULT_MIN_GAP = 20000
BLINK_MIN_GAP_PI = 20000  # cockpit: BLINK at least 20 ms apart
PI_MONITOR = 5000

SRC_PLACE = "assumed: handout placeholder"
SRC_EST = "assumed: estimate from code (SPI bytes / calls)"


class Task:
    def __init__(self, name, prio, C, T, D=None, src=SRC_PLACE, thread=None,
                 report=True, self_backlog=False, rate=None, note="", spi_user=False):
        self.name, self.prio, self.C, self.T, self.D = name, prio, C, T, D
        self.src, self.note = src, note
        self.thread = thread or name
        self.report = report
        self.self_backlog = self_backlog  # FIFO stream: an earlier job may be ahead
        self.rate = rate if rate is not None else 1e6 / T  # jobs per second
        self.spi_user = spi_user
        self.B = 0
        self.R = None


DRIVER = -14  # K_PRIO_COOP(2) with 16 coop priorities: cooperative, above every app thread


def node_tasks(node, scen, k):
    """Part 3: one bus, relay off. Rates: frames this node's MCP2515 sees on one bus."""
    btn_prio = 6 if scen == "proposed" else 5
    rx_per_s = 6 * 50 + (20 if node == "N3" else 0)       # N3 also receives FORCE
    tx_per_s = 2 * 50 + (20 if node == "N2" else 0)       # HB + FAULT (+ FORCE on N2)
    t = [
        Task("MCP2515 driver (RX / TX done)", DRIVER, k.drv_rx, FRAME_MIN, None, SRC_EST,
             thread="drv", self_backlog=True,
             rate=rx_per_s + tx_per_s,
             note=f"RX {k.drv_rx} / TX-done {k.drv_txdone}; SPI {k.hz/1e6:.3g} MHz; <= 1 event per {FRAME_MIN} us"),
        Task("DISPATCH", 4, 15, FRAME_MIN, None, SRC_EST, self_backlog=True, rate=rx_per_s,
             note="dedup, CTRL CRC/range, route"),
    ]
    if node == "N2":
        t += [
            Task("STEER", 5, 50, 20000, 1000, thread="steer", note="on CTRL"),
            Task("STEER (5 ms poll)", 5, 50, ACTUATOR_POLL, None, thread="steer", report=False),
        ]
    else:
        t += [
            Task("DRIVE", 5, 50, 20000, 500, thread="drive", note="on CTRL"),
            Task("DRIVE (5 ms poll)", 5, 50, ACTUATOR_POLL, None, thread="drive", report=False),
        ]
    lamp = "BLINK_F" if node == "N2" else "BLINK_R"
    t += [
        Task(lamp, 5, 20, 20000, 500, thread="blink", note="on BLINK"),
        Task(lamp + " (10 ms poll)", 5, 20, BLINK_POLL, None, thread="blink", report=False),
        Task("TEST_BTN", btn_prio, 45 + k.send, 50000, 10000, SRC_EST, spi_user=True,
             note=f"20 placeholder + zone_update 25 + FAULT send {k.send}"),
    ]
    if node == "N3":
        t.append(Task("PID", 6, 200, 10000, 10000))
    t += [
        Task("HB_TX", 7, 11 + k.send, HB_PERIOD, HB_PERIOD, SRC_EST, spi_user=True,
             note="one bus (template 120 = two buses)"),
        Task("HB_MON", 8, 55 + k.send, MONITOR_PERIOD, MONITOR_PERIOD, SRC_EST,
             spi_user=True, note=f"30 placeholder + zone_update 25 + FAULT send {k.send}; every 5 ms"),
    ]
    if node == "N2":
        t.append(Task("FORCE_TX", 9, 6 + k.send, 50000, 50000, SRC_EST, spi_user=True))
    t += [
        Task("CURRENT", 10, 300 if node == "N2" else 500, 20000, 20000, SRC_EST,
             note=f"{16 if node == 'N2' else 32} blocking adc_read calls, CPU part only"),
        Task("REPORT", 12, 90000, 1000000, 1000000, SRC_EST,
             note="~1000 chars/s of printk, polled UART at 115200"),
    ]
    return t


def by_name(tasks, name):
    return next(x for x in tasks if x.name == name)


def assign_blocking(tasks, scen, k):
    """B_i: the longest time a lower-priority thread can hold something task i needs."""
    spi_users = [x for x in tasks if x.spi_user]
    lowest_spi = max(x.prio for x in spi_users)
    for x in tasks:
        b = B_IRQ
        if x.thread == "drv":
            if scen == "as_built":
                # The SPI lock is a k_sem (no priority inheritance). A low thread
                # holding it mid-transaction can be preempted by every thread
                # between it and the driver: one job of each.
                # (an SPI user among them only runs up to its own send, then
                # blocks on the same k_sem).
                between = {}
                for y in tasks:
                    if y.thread != "drv" and y.prio < lowest_spi:
                        c = y.C - k.send if y.spi_user else y.C
                        between[y.thread] = max(between.get(y.thread, 0), c)
                b = B_IRQ + k.txn_max + sum(between.values())
            else:
                b = B_IRQ + k.send  # k_sched_lock: holder can't be preempted
        else:
            lower_sender = any(y.spi_user and y.prio > x.prio for y in tasks)
            if scen == "proposed" and lower_sender:
                b = max(b, k.send)  # a lower thread's sched-locked send
            if x.thread == "drive":
                b = max(b, B_DRIVE_LOCK)
            if x.name == "TEST_BTN":
                hb_mon = by_name(tasks, "HB_MON")
                upd = 25 + k.send  # update_lock held by HB_MON across its FAULT send
                if scen == "as_built":
                    # plus the SPI k_sem held by HB_TX / HB_MON, preempted by PID
                    mid = sum(y.C for y in tasks if x.prio < y.prio < hb_mon.prio and not y.spi_user)
                    b = upd + k.txn_max + mid
                else:
                    b = upd + k.send
            if x.name in ("HB_TX", "HB_MON") and scen == "as_built":
                if any(y.spi_user and y.prio > x.prio for y in tasks):
                    b = max(b, k.txn_max)
        x.B = round(b)


def rta(tasks, x, limit=10_000_000):
    """R = C + B + sum over higher and equal priority of ceil(R / T) C."""
    others = [y for y in tasks if y is not x and y.thread != x.thread and y.prio <= x.prio]
    same_thread = [y for y in tasks if y is not x and y.thread == x.thread]
    w = x.C + x.B
    while True:
        n = x.C + x.B
        n += sum(math.ceil(w / y.T) * y.C for y in others)
        # another job of the same thread (poll wake-up, earlier frame) may be ahead
        n += sum(y.C for y in same_thread)
        if x.self_backlog:
            n += math.ceil(w / x.T) * x.C
        if n == w or n > limit:
            return n
        w = n


def analyse_node(node, scen, k):
    tasks = node_tasks(node, scen, k)
    assign_blocking(tasks, scen, k)
    for x in tasks:
        x.R = rta(tasks, x)
    return tasks


# ----------------------------------------------------------------------
# Bus
# ----------------------------------------------------------------------

class Msg:
    def __init__(self, mid, name, sender, T, D):
        self.id, self.name, self.sender, self.T, self.D = mid, name, sender, T, D
        self.C = C_FRAME
        self.J = 0
        self.B = C_FRAME  # Davis 2007 sufficient test: B_MAX


def catalog():
    return [
        Msg(0x010, "CTRL", "N1", 20000, 2000),
        Msg(0x020, "FAULT_N1", "N1", 20000, 100000),
        Msg(0x021, "FAULT_N2", "N2", 20000, 100000),
        Msg(0x022, "FAULT_N3", "N3", 20000, 100000),
        Msg(0x100, "HB_N1", "N1", 20000, 20000),
        Msg(0x101, "HB_N2", "N2", 20000, 20000),
        Msg(0x102, "HB_N3", "N3", 20000, 20000),
        Msg(0x200, "BLINK", "N1", 20000, 100000),
        Msg(0x300, "FORCE", "N2", 50000, 50000),
    ]


def local_ahead(m, msgs, scen):
    """Frames of the same sender that can be transmitted before m regardless of ID."""
    own = [k for k in msgs if k.sender == m.sender and k is not m]
    if m.sender == "N1":
        # Linux mcp251x: one TX buffer (TXB0), netdev queue is FIFO.
        if scen == "as_built":
            return own  # timer thread can write FAULT_N1, HB_N1, BLINK just before CTRL
        order = ["CTRL", "FAULT_N1", "HB_N1", "BLINK"]  # all written by the UDP thread, CTRL first
        return [k for k in own if order.index(k.name) < order.index(m.name)]
    # Zephyr MCP2515: three TX buffers, TXP left equal, so the controller sends
    # the highest buffer NUMBER first, not the lowest ID.
    return [k for k in own if k.id > m.id]


def bus_rta(msgs, scen, ideal):
    rows = []
    for m in msgs:
        ahead = [] if ideal else local_ahead(m, msgs, scen)
        top = max([m.id] + [k.id for k in ahead])
        if ideal:
            hp = [j for j in msgs if j.id < m.id]
            local = 0
        else:
            hp = [j for j in msgs if j not in ahead and j is not m and
                  ((j.sender != m.sender and j.id < top) or (j.sender == m.sender and j.id < m.id))]
            dpi = DELTA_PI if m.sender == "N1" else 0
            local = dpi + sum(k.C + dpi for k in ahead)
        w = m.B + local
        while True:
            n = m.B + local + sum(math.ceil((w + j.J + TAU) / j.T) * j.C for j in hp)
            if n == w:
                break
            w = n
        rows.append((m, local, w, m.J + w + m.C))
    return rows


def set_jitter(msgs, nodes):
    n2, n3 = nodes["N2"], nodes["N3"]
    J = {
        "CTRL": PI_TIMER_JITTER,  # stands in for the Pi stage inside interference terms only
        "FAULT_N1": PI_TIMER_JITTER, "HB_N1": PI_TIMER_JITTER, "BLINK": PI_TIMER_JITTER,
        "HB_N2": by_name(n2, "HB_TX").R, "HB_N3": by_name(n3, "HB_TX").R,
        "FAULT_N2": max(by_name(n2, "TEST_BTN").R, by_name(n2, "HB_MON").R),
        "FAULT_N3": max(by_name(n3, "TEST_BTN").R, by_name(n3, "HB_MON").R),
        "FORCE": by_name(n2, "FORCE_TX").R,
    }
    for m in msgs:
        m.J = J[m.name]


# ----------------------------------------------------------------------
# Output
# ----------------------------------------------------------------------

def us(v):
    return f"{v:,.0f}"


def ok(r, d):
    if d is None:
        return ""
    return "yes" if r <= d else "**NO**"


def path_bound(tasks, actuator=None):
    """Upper bound on frame EOF -> end of the actuator job (or end of DISPATCH).

    Two valid bounds, the smaller is reported:
      sum:  R_driver + R_dispatch (+ R_actuator), each stage analysed alone
            (the handout's method; counts interference once per stage)
      busy: the whole path as one job at the lowest priority on it; every
            stage is at that priority or above, so the path is done when that
            busy window ends (interference counted once)
    """
    drv, disp = tasks[0], tasks[1]
    stages = [drv, disp] + ([actuator] if actuator else [])
    s = sum(x.R for x in stages)
    low = stages[-1]
    C = sum(x.C for x in stages)
    B = max(x.B for x in stages)
    skip = {x.thread for x in stages}
    others = [y for y in tasks if y.thread not in skip and y.prio <= low.prio]
    same = [y for y in tasks if y.thread == low.thread and y is not low] if actuator else []
    w = C + B
    while True:
        n = C + B + sum(math.ceil(w / y.T) * y.C for y in others) + sum(y.C for y in same)
        n += math.ceil(w / FRAME_MIN) * (drv.C + disp.C)  # other frames' driver + dispatch jobs
        if n == w:
            break
        w = n
    return min(s, w), s, w


def print_node(node, tasks):
    print(f"\n#### {node}\n")
    print("| Task | Prio | T (us) | C (us) | B (us) | R (us) | D (us) | Met | C: source / note |")
    print("|---|---:|---:|---:|---:|---:|---:|:-:|---|")
    for x in tasks:
        if not x.report:
            continue
        prio = "coop" if x.prio == DRIVER else str(x.prio)
        T = f"{x.T:,}" if not x.self_backlog else f"{x.T} (bus-paced)"
        src = x.src.replace("assumed: ", "") + (f"; {x.note}" if x.note else "")
        print(f"| {x.name} | {prio} | {T} | {us(x.C)} | {us(x.B)} | {us(x.R)} | "
              f"{us(x.D) if x.D else '-'} | {ok(x.R, x.D)} | {src} |")
    drv = tasks[0]
    rx, s, w = path_bound(tasks)
    print(f"| **CAN_RX** = driver + DISPATCH, frame EOF -> handed to task | | | | | {us(rx)} | 300 | {ok(rx, 300)} | min(stage sum {us(s)}, one busy window {us(w)}) |")
    print(f"| **RX overrun check**: driver done before the 3rd frame ends | | | | | {us(drv.R)} | {2*FRAME_MIN} | {ok(drv.R, 2*FRAME_MIN)} | MCP2515 has 2 RX buffers |")
    u = sum(x.C * x.rate for x in tasks) / 1e6
    print(f"\nCPU utilization at actual rates: {100*u:.1f} % (REPORT's polled printk is {100*90000/1e6:.0f} % of it)")


def print_bus(rows_i, rows_b, msgs):
    print("\n| ID | Message | From | T | J | C | B | w (handout) | R (handout) | own-queue term | w (as sent) | R (as sent) | D | Met |")
    print("|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|:-:|")
    for (m, _, wi, ri), (_, loc, wb, rb) in zip(rows_i, rows_b):
        pi = m.name == "CTRL"
        J = "Pi" if pi else us(m.J)
        ri_s = f"Pi + {us(ri - m.J)}" if pi else us(ri)
        rb_s = f"Pi + {us(rb - m.J)}" if pi else us(rb)
        met = ok(rb - (m.J if pi else 0), m.D) if not pi else "see 3.3"
        print(f"| 0x{m.id:03X} | {m.name} | {m.sender} | {m.T:,} | {J} | {m.C} | {m.B} | {us(wi)} | {ri_s} | "
              f"{us(loc)} | {us(wb)} | {rb_s} | {us(m.D)} | {met} |")
    u = sum(m.C / m.T for m in msgs)
    print(f"\nBus utilization, one bus: {100*u:.2f} % (R4.6 ceiling 50 %). All times in us.")


def chains(nodes, rows_b, scen, pi):
    n2, n3 = nodes["N2"], nodes["N3"]
    R = {m.name: (rb - m.J) for (m, _, _, rb) in rows_b}  # queue + w + C, without J
    piv = 0 if pi is None else pi

    def show(title, stages, D, note=""):
        s = sum(v or 0 for _, v, _ in stages)
        print(f"\n**{title}** (deadline {us(D)} us)\n")
        print("| Stage | us | From |")
        print("|---|---:|---|")
        for n, v, src in stages:
            print(f"| {n} | {('Pi' if pi is None else us(pi)) if v is None else us(v)} | {src} |")
        if pi is None:
            print(f"| **Total** | **Pi + {us(s)}** | met if the Pi stage is <= **{us(D - s)}** |")
        else:
            print(f"| **Total** | **{us(s + piv)}** | {ok(s + piv, D)} |")
        if note:
            print(f"\n{note}")
        return s

    pistage = ("Pi stage, UDP_RX -> CMD_TX (measured max)", None, "scope, 3.3")
    b3, b3s, b3w = path_bound(n3, by_name(n3, "DRIVE"))
    show("Brake / throttle: UDP_RX (N1) -> PWM_OUT (N3)", [
        pistage,
        ("CTRL after write(): Pi driver load, Pi queue, bus (own-queue + w + C)", R["CTRL"], "3.2 R as sent - J"),
        ("N3: MCP2515 driver + DISPATCH + DRIVE (to PWM_SET)", b3, f"3.1 N3, min(sum {us(b3s)}, busy window {us(b3w)})"),
        ("PWM compare preload -> PWM_OUT pin", PWM_MOTOR_PERIOD, "10 kHz"),
    ], 2000)

    b2, b2s, b2w = path_bound(n2, by_name(n2, "STEER"))
    show("Steering: UDP_RX (N1) -> SRV (N2)", [
        pistage,
        ("CTRL after write(): Pi driver load, Pi queue, bus", R["CTRL"], "3.2"),
        ("N2: driver + DISPATCH + STEER", b2, f"3.1 N2, min(sum {us(b2s)}, busy window {us(b2w)})"),
        ("Servo compare preload -> new pulse width on SRV", PWM_SERVO_PERIOD, "50 Hz"),
    ], 50000)

    lamp = {"N2": path_bound(n2, by_name(n2, "BLINK_F"))[0],
            "N3": path_bound(n3, by_name(n3, "BLINK_R"))[0]}
    show("Blinker start: UDP_RX (N1) -> FL (N2) / RL (N3)", [
        pistage,
        ("Cockpit timer thread: next 5 ms tick, or 20 ms after the previous BLINK", BLINK_MIN_GAP_PI, "cockpit.c"),
        ("Pi timer-thread wake-up latency", PI_TIMER_JITTER, "assumed"),
        ("BLINK after write(): Pi driver load, Pi queue, bus", R["BLINK"], "3.2"),
        ("Slower node: driver + DISPATCH + BLINK task -> lamp GPIO", max(lamp.values()), "3.1"),
    ], 100000)

    best = {"N2": sum(x.C for x in (n2[0], n2[1], by_name(n2, "BLINK_F"))),
            "N3": sum(x.C for x in (n3[0], n3[1], by_name(n3, "BLINK_R")))}
    skew = max(lamp["N2"] - best["N3"], lamp["N3"] - best["N2"])
    print(f"\n**Blinker sync** (R2.4: FL and RL edges within 1,000 us). One BLINK frame ends at the same "
          f"instant at N2 and N3 (same bus). Skew <= max(WCRT_N2 - BCRT_N3, WCRT_N3 - BCRT_N2) = "
          f"max({us(lamp['N2'])} - {us(best['N3'])}, {us(lamp['N3'])} - {us(best['N2'])}) = "
          f"**{us(skew)} us** -> {ok(skew, 1000)}. (BCRT = the path's C with no interference.)")

    tb = by_name(n2, "TEST_BTN").R
    steer_kick = by_name(n2, "STEER").R
    rx3 = path_bound(n3)[0]
    react3 = rx3 + MONITOR_PERIOD + by_name(n3, "HB_MON").R + ACTUATOR_POLL + by_name(n3, "DRIVE").R
    lamps = max(lamp.values())
    hazard_from_n1_rx = PI_TIMER_JITTER + PI_MONITOR + BLINK_MIN_GAP_PI + R["BLINK"] + lamps
    rows = [
        ("Local fail on N2: TEST_BTN (zone_update sets ERR) + STEER wakes and centers", tb + steer_kick, 10000),
        ("N3 brakes, via FAULT_N2: TEST_BTN + bus + N3 RX + next HB_MON tick + next DRIVE poll", tb + R["FAULT_N2"] + react3, 100000),
        ("  same, FAULT_N2 held by the 20 ms FAULT spacing", tb + FAULT_MIN_GAP + MONITOR_PERIOD + R["FAULT_N2"] + react3, 100000),
        ("N3 brakes, via the next HB_N2 carrying LOCAL_FAULT (FAULT frame lost)", tb + HB_PERIOD + R["HB_N2"] + react3, 100000),
        ("Hazards on FL/RL, via FAULT_N2 -> cockpit monitor -> hazard BLINK", tb + R["FAULT_N2"] + hazard_from_n1_rx, 100000),
        ("Hazards on FL/RL, via HB_N2 (FAULT frame lost)", tb + HB_PERIOD + R["HB_N2"] + hazard_from_n1_rx, 100000),
        ("Zone dead (handout chain, checkoff 8): N3 brakes after 66 ms silence", FAIL_TIMEOUT + MONITOR_PERIOD + by_name(n3, "HB_MON").R + ACTUATOR_POLL + by_name(n3, "DRIVE").R, 100000),
        ("Zone dead: hazards on FL/RL via the cockpit", FAIL_TIMEOUT + hazard_from_n1_rx, 100000),
    ]
    print("\n**Error propagation, test button on N2** (R2.5: local fail 10 ms, system error 100 ms)\n")
    print("| Path | us | D | Met |")
    print("|---|---:|---:|:-:|")
    for n, v, d in rows:
        print(f"| {n} | {us(v)} | {us(d)} | {ok(v, d)} |")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--scenario", choices=["as_built", "proposed", "both"], default="both")
    ap.add_argument("--pi-us", type=float, default=None,
                    help="measured Pi stage, UDP_RX -> CMD_TX worst case (us)")
    a = ap.parse_args()
    scens = ["as_built", "proposed"] if a.scenario == "both" else [a.scenario]

    for scen in scens:
        k = Costs(scen)
        print(f"\n\n## Scenario: {scen}\n")
        print(f"SPI {k.hz/1e6:.3f} MHz ({8e6/k.hz:.2f} us/byte on the wire; one received frame = "
              f"17 bytes = {k.wire_rx:.0f} us of wire time). Derived: driver RX job {k.drv_rx} us, "
              f"TX-done job {k.drv_txdone} us, send on one bus {k.send} us, longest SPI transaction "
              f"{k.txn_max} us.")
        nodes = {n: analyse_node(n, scen, k) for n in ("N2", "N3")}
        print("\n### 3.1 Node response times")
        for n in ("N2", "N3"):
            print_node(n, nodes[n])
        msgs = catalog()
        set_jitter(msgs, nodes)
        print("\n### 3.2 Message response times (one bus)")
        rows_b = bus_rta(msgs, scen, False)
        print_bus(bus_rta(msgs, scen, True), rows_b, msgs)
        print("\n### 3.3 End-to-end chains")
        chains(nodes, rows_b, scen, a.pi_us)


if __name__ == "__main__":
    main()
