# Test Vectors

Each script in `mac/` sends UDP packets from the laptop to the Pi, which forwards them to the STM32.
Run one script at a time.

## `send_steer.py`: hold a steering value

Keeps sending the same state every 50 ms until Ctrl-C.

| Usage | What it does |
|---|---|
| `python3 mac/send_steer.py 0` | Centers the steering (90°) |
| `python3 mac/send_steer.py -10000` | Full left lock (0°) |
| `python3 mac/send_steer.py 5000 --hold-ms 2000` | Half right (135°) for 2 s, then stops, so the car hits `LINK_TIMEOUT` |
| `python3 mac/send_steer.py --norm 0.5` | Treats the value as −1…1 (0.5 → 16383), which saturates to full right lock (180°) |
| `python3 mac/send_steer.py 40000` | Sends an out-of-range value: `BAD_RANGE` fault, motors brake, hazards on |

Extra options: `--throttle N`, `--brake N`, `--button N` (repeatable), `--counter N`.

## `send_throttle_brake.py`: throttle, then brake

**The wheels spin. Put the car on a stand.**

| Usage | What it does |
|---|---|
| `python3 mac/send_throttle_brake.py` | Half throttle for 3.75 s (≈322 mm/s), then brake on top of throttle for 2.5 s (brake wins, wheels stop), then exits |
| `python3 mac/send_throttle_brake.py --throttle 32767 --throttle-packets 100 --brake-packets 50` | Full throttle (650 mm/s) for 5 s, then full brake for 2.5 s |

## `send_left_signal.py`: tap the left turn signal

| Usage | What it does |
|---|---|
| `python3 mac/send_left_signal.py` | Taps button 5 once, which toggles the left signal (on, or off if it was already on), then keeps the link alive until Ctrl-C |
| `python3 mac/send_left_signal.py --steer -3000` | Same, with steering held at −3000 |

## `send_self_test.py`: enter self-test

| Usage | What it does |
|---|---|
| `python3 mac/send_self_test.py` | Sends 5 neutral packets, then presses button 10 to enter `SELF_TEST`, then exits |
| `python3 mac/send_self_test.py --count 10 --interval-ms 100` | Same, with 10 neutral packets at 100 ms spacing |

## `mac_proxy.py`: drive with the Logitech wheel

| Usage | What it does |
|---|---|
| `python3 mac/mac_proxy.py` | Streams the live wheel, pedals and buttons to the car every 20 ms |
| Hold wheel button 1 while running | Injects a fault (steer −40000, throttle 40000): `BAD_RANGE`, brake, hazards. Release to recover |
| `python3 mac/mac_proxy.py --probe` | Prints live axis values and exits |
