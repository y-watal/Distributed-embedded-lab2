# Lab 3 pin map

Test points follow the row order in the Lab 3 handout ("Test points and fault
injection"). Each node gets its own small breadboard, one signal per row,
labeled on tape. LED rows (DEG, blinkers) drive an LED through a ~330 ohm
resistor to GND, and the row doubles as the test point.

`ERR` has a row on every node (N1 row 9, N3 row 14) even though the handout
table lists it only for N2: checkoff step 1 and step 9 expect ERR high on all
three nodes.

## Test-point breadboards

| Row | N1 Pi: signal | Pi GPIO (header pin) | N2 front: signal | Nucleo pin | N3 rear: signal | Nucleo pin |
|---:|---|---|---|---|---|---|
| 1 | GND | GND (pin 39) | GND | GND | GND | GND |
| 2 | GND | GND (pin 34) | GND | GND | GND | GND |
| 3 | UDP_RX | GPIO5 (29) | CAN_RX_A | A0 / PA0 | CAN_RX_A | A0 / PA0 |
| 4 | CMD_TX | GPIO6 (31) | CAN_RX_B | A1 / PA1 | CAN_RX_B | A1 / PA1 |
| 5 | CAN_TX_A | GPIO13 (33) | CAN_TX_A | A2 / PA4 | CAN_TX_A | A2 / PA4 |
| 6 | CAN_TX_B | GPIO19 (35) | CAN_TX_B | A3 / PB0 | CAN_TX_B | A3 / PB0 |
| 7 | DEG_A (LED) | GPIO26 (37) | DEG_A (LED) | A4 / PC1 | DEG_A (LED) | A4 / PC1 |
| 8 | DEG_B (LED) | GPIO16 (36) | DEG_B (LED) | A5 / PC0 | DEG_B (LED) | A5 / PC0 |
| 9 | ERR *(added)* | GPIO20 (38) | SRV (servo PWM) | D5 / PB4, TIM3_CH1 | PWM_SET | D6 / PB10 |
| 10 | - | | FL (LED) | D6 / PB10 | PWM_OUT (motor PWM) | D5 / PB4, TIM3_CH1 |
| 11 | - | | FR (LED) | D7 / PA8 | DIR_A | D7 / PA8 |
| 12 | - | | RELAY_TX | D8 / PA9 | RL (LED) | D8 / PA9 |
| 13 | - | | ERR | D3 / PB3 | RR (LED) | D14 / PB9 |
| 14 | | | | | ERR *(added)* | D3 / PB3 |

Both Nucleos use the same pins for rows 3-8 (the A0-A5 analog header, in row
order), the same pin for ERR (D3) and the same pin for their PWM output (D5).
The test button is the on-board blue USER button B1 (PC13) on both Nucleos.

### What each point does

| Point | Toggles / level |
|---|---|
| UDP_RX, CMD_TX | Toggle on reception of a UDP update and on the command's CAN write |
| CAN_TX_A / CAN_TX_B | Toggle on the call that queues a frame on that controller |
| CAN_RX_A / CAN_RX_B | Toggle when dispatch sees a frame from that controller, after the SPI read |
| RELAY_TX | Toggle when the relay queues a forwarded frame on the other bus |
| DEG_A / DEG_B | On while that bus is degraded at this node (R4.7) |
| ERR | High while the zone is in the error state |
| SRV / PWM_OUT | The servo (50 Hz) and motor (10 kHz) PWM signals themselves |
| PWM_SET | Toggles when a new motor duty is written |
| DIR_A | H-bridge direction level |
| FL, FR, RL, RR | The blinker LED outputs |

In code: `node2_front/src/tp.h` and `node3_rear/src/tp.h` (`tp_toggle(TP_CAN_TX_A)`,
`tp_set(TP_ERR, 1)`), with pins in each node's `app.overlay`; on the Pi,
`node1_pi/testpoints.py` (Python) and `node1_pi/testpoints.h` (C, /dev/gpiomem).
The bring-up firmware and `can_test.py` walk every point high for 300 ms at
start-up so the wiring can be checked with an LED or a scope.

## CAN modules

Module pins are XIAO names. The handout table has MOSI and MISO swapped on
the module side; on the XIAO footprint D9 is MISO and D10 is MOSI.

| Module pin | Function | N1 Pi, can0 (A) | N1 Pi, can1 (B) | N2/N3 CAN A | N2/N3 CAN B |
|---|---|---|---|---|---|
| 3V3 / GND | Power, 3.3 V only | pin 1 or 17 / GND | pin 1 or 17 / GND | 3V3 / GND | 3V3 / GND |
| D8 | SCK | GPIO11 (23) | GPIO11 (23) | D13 / PA5 | D13 / PA5 |
| D10 | MOSI | GPIO10 (19) | GPIO10 (19) | D11 / PA7 | D11 / PA7 |
| D9 | MISO | GPIO9 (21) | GPIO9 (21) | D12 / PA6 | D12 / PA6 |
| D7 | CS | GPIO8 / CE0 (24) | GPIO7 / CE1 (26) | D10 / PB6 | D9 / PC7 |
| D6 | INT | GPIO25 (22) | GPIO24 (18) | D2 / PA10 | D4 / PB5 |
| CANH / CANL | Bus | bus A rails | bus B rails | bus A rails | bus B rails |

## Nucleo pins that are taken

| Pins | Used by |
|---|---|
| PA2, PA3 (D1, D0) | USART2 console (ST-LINK virtual COM port) |
| PA5, PA6, PA7 (D13-D11) | SPI1 to both CAN modules |
| PB6, PC7 (D10, D9) | CAN A / CAN B chip select |
| PA10, PB5 (D2, D4) | CAN A / CAN B interrupt |
| PA13, PA14 | SWD (ST-LINK) |
| PC13 | USER button B1 (test button) |
| Free on the Arduino headers | D15 / PB8 (both), D14 / PB9 (N2 only) |

The overlays disable board defaults that would otherwise claim these pins:
USART1 (PB6/PB7), I2C1 (PB8/PB9), I2C3 (PA8/PC9), ADC1 (PA0) and the TIM2 PWM
LED on PA5. The on-board green LED LD2 shares PA5 with SPI1 SCK and can't be used.
