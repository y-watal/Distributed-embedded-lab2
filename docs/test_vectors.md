# UDP Test Vectors (Laptop → Pi)

All test vectors are sent from the laptop by the scripts in `mac/` to the Pi's
`pi/uart_receiver.c`, which forwards the relevant fields to the STM32 over UART
(see `docs/protocol.md`).

## Transport

| Item | Value |
|---|---|
| Destination | Pi `172.26.74.45`, UDP port `8765` (`--remote`, `--send-port`) |
| Return path (force feedback, `mac_proxy.py` only) | Laptop `172.26.2.109`, UDP port `8001` |
| Packet size | 276 bytes: 4-byte counter + 272-byte `DIJOYSTATE2` |
| Byte order | Little-endian |
| Default period | 50 ms for `send_*.py`, 20 ms for `mac_proxy.py` |

## Packet layout

| UDP offset | Size | Field | Value sent |
|---:|---:|---|---|
| 0 | u32 | Packet counter | `mac_proxy.py`: starts at 0. `send_*.py`: seeded from the clock in ms, +1 per packet |
| 4 | i32 | `lX` (steering) | **Forwarded** |
| 8 | i32 | `lY` (throttle) | **Forwarded** |
| 12, 16, 20 | i32 | `lZ`, `lRx`, `lRy` | 0 |
| 24 | i32 | `lRz` (brake) | **Forwarded** |
| 28 | 2 × i32 | Sliders | 0 |
| 36 | 4 × u32 | POV hats | `0xFFFFFFFF` (centered) |
| 52 | 128 × u8 | Buttons (`0x80` = pressed) | Only button 4 (offset 56), 5 (offset 57) and 10 (offset 62) are forwarded |
| 180 | 24 × i32 | Velocity / acceleration / force | 0 |

Example: a neutral packet with counter `0x00000001` is
`01 00 00 00` + 32 zero bytes + 16 × `FF` + 128 zero bytes + 96 zero bytes.

## What the STM32 accepts

| Field | Accepted range | Meaning |
|---|---|---|
| Steering `lX` | −32767 … 32766 | Negative = left. Saturates at ±10000 (full lock): −10000 → 0°, 0 → 90°, +10000 → 180° servo |
| Throttle `lY` | 0 … 32767 | ≤ 300 is deadband (coast). 32767 → 650 mm/s target |
| Brake `lRz` | 0 … 32767 | ≥ 500 brakes and overrides throttle |
| Buttons 4 / 5 / 10 | 0 or 128 | 4 = right signal, 5 = left signal, 10 = self-test (rising edge) |

Anything out of range is rejected: motors brake, hazards start, fault = `BAD_RANGE`.
No valid command for 90 ms gives `LINK_TIMEOUT`.

---

## Test vectors

### TV-1: Steering hold (`send_steer.py`)

Sends one fixed state repeatedly every 50 ms until Ctrl-C (or `--hold-ms`).
Throttle, brake and buttons default to 0.

| ID | Command | lX | lY | lRz | Buttons | Duration | Expected result |
|---|---|---:|---:|---:|---|---|---|
| TV-1a | `send_steer.py 0` | 0 | 0 | 0 | none | Until Ctrl-C | Servo centered (90°), NORMAL |
| TV-1b | `send_steer.py -10000` | −10000 | 0 | 0 | none | Until Ctrl-C | Full left lock (0°) |
| TV-1c | `send_steer.py 5000 --hold-ms 2000` | 5000 | 0 | 0 | none | 2 s (~40 packets), then stop | Half right (135°) for 2 s, then `LINK_TIMEOUT` |
| TV-1d | `send_steer.py --norm 0.5` | 16383 | 0 | 0 | none | Until Ctrl-C | Past ±10000, so full right lock (180°) |
| TV-1e | `send_steer.py 40000` | 40000 | 0 | 0 | none | Until Ctrl-C | Out of range: rejected, `BAD_RANGE`, motors braked, hazards on |

`send_steer.py` also accepts `--throttle`, `--brake`, `--button N` (repeatable)
and `--counter`, so any fixed state can be held. It warns but still sends
values outside the accepted range.

### TV-2: Throttle, then brake over throttle (`send_throttle_brake.py`)

Steering 0, no buttons, 50 ms period. The script exits after the last packet.

| Phase | Packets | Time | lX | lY | lRz | Expected result |
|---|---:|---|---:|---:|---:|---|
| 1. Throttle | 75 | 0 – 3.75 s | 0 | 16384 | 0 | Wheels spin, PID target ≈ 322 mm/s |
| 2. Brake + throttle | 50 | 3.75 – 6.25 s | 0 | 16384 | 32767 | Brake wins (≥ 500): wheels stop with throttle still held |
| 3. Stop sending | 0 | +90 ms | — | — | — | `LINK_TIMEOUT`, motors stay braked |

Variant from the docstring:
`send_throttle_brake.py --throttle 32767 --throttle-packets 100 --brake-packets 50`
sends full throttle (650 mm/s target) for 5 s, then full brake for 2.5 s.

**The wheels spin. Put the car on a stand.**

### TV-3: Left turn signal (`send_left_signal.py`)

Steering held at 0 (or `--steer`), pedals 0, 50 ms period.

| Phase | Time | lX | lY | lRz | Button 5 | Expected result |
|---|---|---:|---:|---:|---:|---|
| 1. Settle | 0 – 200 ms (~4 packets) | 0 | 0 | 0 | 0 | Link restored, STM32 sees button released |
| 2. Press | 200 – 300 ms (~2 packets) | 0 | 0 | 0 | 128 | Rising edge: left indicator on |
| 3. Release | 300 ms until Ctrl-C (or `--hold-ms`) | 0 | 0 | 0 | 0 | Left indicator keeps blinking; link kept alive |

Notes:
- Running it again while the left signal is on toggles it **off**.
- The left signal auto-cancels only after steering goes to ≤ −5000 and then returns to ≥ −4000, so holding 0 leaves it on.
- Variant: `send_left_signal.py --steer -3000`.

### TV-4: Self-test entry (`send_self_test.py`)

Steering and pedals 0, 50 ms period. The script exits after the last packet.

| Packet | Time | lX | lY | lRz | Button 10 | Expected result |
|---:|---|---:|---:|---:|---:|---|
| 1 – 5 | 0 – 200 ms | 0 | 0 | 0 | 0 | First valid frame moves ERROR → NORMAL; button seen released |
| 6 | 250 ms | 0 | 0 | 0 | 128 | Rising edge in NORMAL requests `SELF_TEST` |
| — | ~340 ms | — | — | — | — | Nothing more sent: `LINK_TIMEOUT` |

Variant: `send_self_test.py --count 10 --interval-ms 100`.
Exiting self-test needs a double press of button 10 within 350 ms, which this
script does not send.

### TV-5: Live wheel with fault injection (`mac_proxy.py`)

Reads the Logitech wheel through pygame and sends every 20 ms, counter from 0.

| Condition | lX | lY | lRz | Buttons | Expected result |
|---|---|---|---|---|---|
| Normal driving | wheel × 32767 (−32767 … 32767) | pedal × 32767 (0 … 32767) | pedal × 32767 (0 … 32767) | Live wheel buttons as `0x80` / `0x00` | Normal operation |
| Fault button (button 1) held | **−40000** (`--fault-steer`) | **40000** (`--fault-throttle`) | Live pedal | Live | Out of range: `BAD_RANGE`, brake + hazards. Release to recover |

Note: at full right, `lX` = +32767, one above the STM32's 32766 limit, so
the wheel hard against its right stop is also rejected as `BAD_RANGE`.

---

## Pi forwarding note

`pi/uart_receiver.c` keeps the latest UDP packet and forwards it to the STM32
at most once every 10 ms. Packets shorter than 276 bytes are dropped. Keep the
laptop period above 10 ms or some states never reach the STM32.

`docs/protocol.md` describes change-only forwarding with a 50 ms keepalive,
but the current `uart_receiver.c` does not do that. It only applies the 10 ms
rate limit.

Only run one sender at a time. Packets from two scripts overwrite each other on the Pi.
