"""Lab 3 message catalog for the Pi, matching node2_front/src/catalog.h.

Byte 0 of every payload: low nibble = per-ID 4-bit sequence counter, high
nibble = flags (0). Full table: docs/lab3_design.md.
"""
import struct

N1, N2, N3 = 0, 1, 2

ID_CTRL = 0x010
ID_BLINK = 0x200
ID_FORCE = 0x300


def ID_FAULT(n):  # noqa: N802
    return 0x020 + n


def ID_HB(n):  # noqa: N802
    return 0x100 + n


SENDER = {ID_CTRL: N1, ID_BLINK: N1, ID_FORCE: N2,
          **{ID_HB(n): n for n in (N1, N2, N3)},
          **{ID_FAULT(n): n for n in (N1, N2, N3)}}

# CTRL (handout template layout)
CTRL_STEER_MIN, CTRL_STEER_MAX = -32767, 32766   # Lab 2 raw steering
CTRL_PEDAL_SHIFT = 7                             # Lab 2 raw pedal (0..32767) >> 7 -> 0..255
CTRL_BTN4, CTRL_BTN5, CTRL_BTN10 = 0x01, 0x02, 0x04


def crc8_j1850(data):
    """CRC-8/SAE-J1850: poly 0x1D, init 0xFF, final XOR 0xFF ("123456789" -> 0x4B)."""
    crc = 0xFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1D) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc ^ 0xFF


def ctrl_payload(seq, steer, throttle, brake, buttons):
    """CTRL payload, 8 bytes. steer is the int16 value, throttle/brake are payload
    counts 0..255, buttons the CTRL_BTN_* bits. Values outside the valid range
    are encoded as given (for fault injection), so clamp before calling."""
    data = bytearray(8)
    data[0] = seq & 0x0F
    struct.pack_into("<h", data, 1, steer)
    data[3] = throttle & 0xFF
    data[4] = brake & 0xFF
    data[5] = buttons & 0xFF
    data[6] = crc8_j1850(data[0:6])
    data[7] = 0
    return bytes(data)


def ctrl_from_lab2(raw_steer, raw_throttle, raw_brake, btn4, btn5, btn10):
    """Maps Lab 2's raw wheel values onto CTRL fields: (steer, throttle, brake, buttons).

    Steering outside Lab 2's valid range is sent as -32768, which receivers reject
    as CTRL_RANGE (an out-of-range wheel value must not wrap into a valid one).
    Pedals outside 0..32767 have no out-of-range encoding in a uint8, so they are
    clamped; use steering to inject a range fault."""
    steer = raw_steer if CTRL_STEER_MIN <= raw_steer <= CTRL_STEER_MAX else -32768
    throttle = min(max(raw_throttle, 0), 32767) >> CTRL_PEDAL_SHIFT
    brake = min(max(raw_brake, 0), 32767) >> CTRL_PEDAL_SHIFT
    buttons = (CTRL_BTN4 if btn4 else 0) | (CTRL_BTN5 if btn5 else 0) | (CTRL_BTN10 if btn10 else 0)
    return steer, throttle, brake, buttons
