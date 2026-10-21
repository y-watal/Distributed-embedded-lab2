# N3 - rear Nucleo: CAN bring-up test

Sends `HB_N3` (`0x102`) every 100 ms on both CAN modules and prints a
once-a-second receive summary on the ST-LINK virtual COM port (115200 baud).
N2 is the same code with `NODE_NAME`/`HB_ID` changed at the top of `src/main.c`.

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

## Build and flash

```
west build -b nucleo_f401re node3_rear -d node3_rear/build
west flash -d node3_rear/build
```

With both Nucleos plugged into the same laptop, pick the board by its
ST-LINK serial: `west flash -d node3_rear/build --dev-id <serial>`
(list serials with `st-info --probe` or `STM32_Programmer_CLI -l`).

## What to expect

```
CAN A (can@0): started
CAN B (can@1): started
[N3 12.000s] rx dropped 0
  A: error-active tec=0 rec=0 | tx ok=10 err=0 full=0 | rx: 0x100 x10 0x101 x10
  B: error-active tec=0 rec=0 | tx ok=10 err=0 full=0 | rx: 0x100 x10 0x101 x10
```

- `not ready` at boot: SPI wiring, CS pin, or no 3V3 to the module.
- `tx err`/`full` climbing and `tec` rising to error-passive: nobody ACKs,
  so the bus isn't connected, there's no termination (measure 60 ohm
  CANH-CANL powered off), or another node runs a different bitrate/oscillator.
- IDs at x5 or x20 instead of x10: an `osc-freq`/`oscillator=` mismatch.
