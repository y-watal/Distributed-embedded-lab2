#!/usr/bin/env python3
"""Lab 3 test points on the Pi (N1), BCM GPIO numbering.

Row numbers are the test-point breadboard rows (rows 1-2 are GND, from header
pins 39 and 34). Full map: docs/lab3_pin_map.md. Uses lgpio, which ships with
Raspberry Pi OS (sudo apt install python3-lgpio if it's missing). Without it,
every call is a no-op so the CAN code still runs.

Run this file directly to walk every point high in turn:  ./testpoints.py
"""
import time

# name: (BCM GPIO, header pin, breadboard row)
TEST_POINTS = {
    "UDP_RX":   (5,  29, 3),   # reception of a UDP update
    "CMD_TX":   (6,  31, 4),   # transmission of the command (the CAN write)
    "CAN_TX_A": (13, 33, 5),   # call that queues a frame on can0
    "CAN_TX_B": (19, 35, 6),   # call that queues a frame on can1
    "DEG_A":    (26, 37, 7),   # LED: bus A degraded at this node
    "DEG_B":    (16, 36, 8),   # LED: bus B degraded at this node
    "ERR":      (20, 38, 9),   # high while the cockpit zone is in the error state (added row)
}


class TestPoints:
    def __init__(self, chip=0):
        self._lg = None
        self._h = None
        self._level = {name: 0 for name in TEST_POINTS}
        try:
            import lgpio
        except ImportError:
            print("testpoints: lgpio not installed, test points disabled")
            return
        try:
            self._h = lgpio.gpiochip_open(chip)
            for gpio, _, _ in TEST_POINTS.values():
                lgpio.gpio_claim_output(self._h, gpio, 0)
            self._lg = lgpio
        except Exception as e:  # lgpio raises its own error type
            print(f"testpoints: GPIO unavailable ({e}), test points disabled")
            if self._h is not None:
                lgpio.gpiochip_close(self._h)
            self._h = None

    def set(self, name, level):
        level = 1 if level else 0
        self._level[name] = level
        if self._lg:
            self._lg.gpio_write(self._h, TEST_POINTS[name][0], level)

    def toggle(self, name):
        self.set(name, not self._level[name])

    def close(self):
        if self._lg:
            for name in TEST_POINTS:
                self.set(name, 0)
            self._lg.gpiochip_close(self._h)
            self._lg = None


def walk(ms_per_point=300):
    tp = TestPoints()
    try:
        for name, (gpio, pin, row) in TEST_POINTS.items():
            print(f"row {row:2d}  {name:9s} GPIO{gpio:<2d} (header pin {pin})")
            tp.set(name, 1)
            time.sleep(ms_per_point / 1000)
            tp.set(name, 0)
    finally:
        tp.close()


if __name__ == "__main__":
    walk()
