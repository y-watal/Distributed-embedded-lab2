# N2 - front Nucleo: CAN bring-up test

Sends `HB_N2` (`0x101`) every 20 ms on both CAN modules and prints a
once-a-second receive summary on the ST-LINK virtual COM port (115200 baud).
N3 is the same code with `NODE_NAME`/`HB_ID` changed at the top of `src/main.c`.

## Wiring (Nucleo Arduino header -> XIAO CAN module)

| Module pin | Function | CAN A module | CAN B module |
|---|---|---|---|
| 3V3 / GND | Power (3.3 V only) | 3V3 / GND | 3V3 / GND |
| D8 | SCK | D13 (PA5) | D13 (PA5) |
| D10 | MOSI | D11 (PA7) | D11 (PA7) |
| D9 | MISO | D12 (PA6) | D12 (PA6) |
| D7 | CS | D10 (PB6) | D9 (PC7) |
| D6 | INT | D2 (PA10) | D4 (PB5) |

The Lab 3 handout's table has MOSI/MISO swapped on the module side; the
XIAO footprint is D9 = MISO, D10 = MOSI. If you change a pin, change
`app.overlay` to match. Check `osc-freq` against the crystal on the module.

## Test points

Pins and breadboard rows are in `docs/lab3_pin_map.md`; the pins are set in
`app.overlay`, and the code calls `tp_toggle()` / `tp_set()` from `src/tp.h`.
At boot the firmware drives every test point high for 300 ms in turn and
prints its name, so each row can be checked with an LED or a scope. Set
`TP_WALK_MS` to 0 in `src/main.c` to skip it. After that:

- CAN_TX_A/B toggle on every send, and CAN_RX_A/B on every received frame.
- DEG_A/B are on while no other node's heartbeat has been heard on that bus for 60 ms.
- ERR is on at power-up, and while another node is missing on both buses.
- The PWM output idles at `PWM_IDLE_US`: SRV holds 1.5 ms servo-center pulses. The remaining points are only driven by the boot walk.

## Build and flash

```
west build -b nucleo_f401re node2_front -d node2_front/build
west flash -d node2_front/build
```

With both Nucleos plugged into the same laptop, pick the board by its
ST-LINK serial: `west flash -d node2_front/build --dev-id <serial>`
(list serials with `st-info --probe` or `STM32_Programmer_CLI -l`).

## What to expect

```
CAN A (can@0): started
CAN B (can@1): started
[N2 12.000s] rx dropped 0 | ERR 0
  A: error-active tec=0 rec=0 DEG 0 | tx ok=50 err=0 full=0 | rx: 0x100 x50 0x102 x50
  B: error-active tec=0 rec=0 DEG 0 | tx ok=50 err=0 full=0 | rx: 0x100 x50 0x102 x50
```

- `not ready` at boot: SPI wiring, CS pin, or no 3V3 to the module.
- `tx err`/`full` climbing and `tec` rising to error-passive: nobody ACKs,
  so the bus isn't connected, there's no termination (measure 60 ohm
  CANH-CANL powered off), or another node runs a different bitrate/oscillator.
- IDs at x25 or x100 instead of x50: an `osc-freq`/`oscillator=` mismatch.
