# Lab 3 pin map

Test points follow the row order in the Lab 3 handout ("Test points and fault
injection"). Each node gets its own small breadboard, one signal per row,
labeled on tape. LED rows (DEG, blinkers) drive an LED through a ~330 ohm
resistor to GND, and the row doubles as the test point.

The Lab 2 actuators and sensors stay on their **Lab 2 pins**, so the car's
wiring carries over (except the blinker LEDs and the Pi UART, whose Lab 2
pins now belong to the CAN modules). The test points that collided with them
moved instead. Rows that are actuator signals (PWM_OUT, DIR_A) are a
second wire from that pin to the breadboard row.

`ERR` has a row on every node (N1 row 9, N3 row 14) even though the handout
table lists it only for N2: checkoff steps 1 and 9 expect ERR high on all
three nodes.

## Test-point breadboards

| Row | N1 Pi: signal | Pi GPIO (header pin) | N2 front: signal | Nucleo pin | N3 rear: signal | Nucleo pin |
|---:|---|---|---|---|---|---|
| 1 | GND | GND (pin 39) | GND | GND | GND | GND |
| 2 | GND | GND (pin 34) | GND | GND | GND | GND |
| 3 | UDP_RX | GPIO5 (29) | CAN_RX_A | A0 / PA0 | CAN_RX_A | PC2 (morpho) |
| 4 | CMD_TX | GPIO6 (31) | CAN_RX_B | A1 / PA1 | CAN_RX_B | PC3 (morpho) |
| 5 | CAN_TX_A | GPIO13 (33) | CAN_TX_A | A2 / PA4 | CAN_TX_A | D15 / PB8 |
| 6 | CAN_TX_B | GPIO19 (35) | CAN_TX_B | A3 / PB0 | CAN_TX_B | D14 / PB9 |
| 7 | DEG_A (LED) | GPIO26 (37) | DEG_A (LED) | D15 / PB8 | DEG_A (LED) | A4 / PC1 |
| 8 | DEG_B (LED) | GPIO16 (36) | DEG_B (LED) | A5 / PC0 | DEG_B (LED) | A5 / PC0 |
| 9 | ERR *(added)* | GPIO20 (38) | SRV | D14 / PB9 | PWM_SET | D6 / PB10 |
| 10 | - | | FL (LED) | D6 / PB10 | PWM_OUT = left motor ENA | A0 / PA0 |
| 11 | - | | FR (LED) | D7 / PA8 | DIR_A = H-bridge IN1 | PB12 (morpho) |
| 12 | - | | RELAY_TX | D8 / PA9 | RL (LED) | D5 / PB4 |
| 13 | - | | ERR | D3 / PB3 | RR (LED) | PC8 (morpho) |
| 14 | | | | | ERR *(added)* | PC6 (morpho) |

"Morpho" pins are on the two long ST morpho headers; their names are printed
next to the header on the Nucleo. The test button is the on-board blue USER
button B1 (PC13) on both Nucleos.

### What each point does

| Point | Toggles / level |
|---|---|
| UDP_RX, CMD_TX | Toggle on reception of a UDP update and on the command's CAN write |
| CAN_TX_A / CAN_TX_B | Toggle on the call that queues a frame on that controller |
| CAN_RX_A / CAN_RX_B | Toggle when dispatch sees a frame from that controller, after the SPI read |
| RELAY_TX | Toggle when the relay queues a forwarded frame on the other bus |
| DEG_A / DEG_B | On while that bus is degraded at this node (R4.7) |
| ERR | High while the zone is in the error state |
| SRV | N2. Toggles every time the servo duty is set: once per CTRL STEER acts on (same angle or not) and when it re-centers. The servo PWM itself stays on D5 / PB4 (50 Hz, 0.5-2.5 ms) |
| PWM_OUT | The left motor's enable PWM itself (10 kHz) |
| PWM_SET | Toggles every time the motor is set: a duty (throttle; 0 = coast) or the brake, whether or not the output changed. DRIVE sets it once per CTRL it acts on and once when it drops to OFF, so with the PID compiled out that is one edge per CTRL that reaches the motor |
| DIR_A | H-bridge IN1: low for forward drive and for brake (Lab 2 wiring) |
| FL, FR, RL, RR | The blinker LED outputs |

In code: `src/tp.h` (`tp_toggle(TP_CAN_TX_A)`, `tp_set(TP_ERR, 1)`) with pins
in each node's `app.overlay`; SRV is toggled in `servo.c`; PWM_OUT and DIR_A
are driven by `motor.c`. On the Pi, `node1_pi/testpoints.py` (Python) and
`node1_pi/testpoints.h` (C, /dev/gpiomem). The firmware and `can_test.py` walk
every GPIO test point high for 300 ms at start-up so the wiring can be checked.

## Lab 2 actuators and sensors

| Signal | Node | Nucleo pin | Peripheral |
|---|---|---|---|
| Steering servo | N2 | D5 / PB4 | TIM3_CH1, 50 Hz |
| Servo current sensor | N2 | A4 / PC1 | ADC1_IN11 |
| Left motor enable (ENA) | N3 | A0 / PA0 | TIM5_CH1, 10 kHz |
| Right motor enable (ENB) | N3 | A1 / PA1 | TIM5_CH2, 10 kHz |
| H-bridge IN1 / IN2 / IN3 / IN4 | N3 | PB12 / PB13 / PB14 / PB15 (morpho) | GPIO |
| Left encoder A / B | N3 | D7 / PA8, D8 / PA9 | TIM1 quadrature |
| Right encoder A / B | N3 | PA15 (morpho), D3 / PB3 | TIM2 quadrature |
| Left motor current sensor | N3 | A3 / PB0 | ADC1_IN8 |
| Right motor current sensor | N3 | A2 / PA4 | ADC1_IN4 |

Blinker LEDs moved from their Lab 2 pins (PB5, PB10, PC7, PA10 are now CAN
interrupt / chip-select lines): FL D6 / PB10 and FR D7 / PA8 on N2, RL D5 / PB4
and RR PC8 on N3.

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

## Nucleo pins in use

| Pins | N2 front | N3 rear |
|---|---|---|
| PA2, PA3 (D1, D0) | USART2 console | USART2 console |
| PA5, PA6, PA7 | SPI1 to both CAN modules | SPI1 to both CAN modules |
| PB6, PC7 / PA10, PB5 | CAN A / B chip select / interrupt | same |
| PA13, PA14 | SWD (ST-LINK) | SWD |
| PC13 | USER button B1 | USER button B1 |
| Free on the Arduino headers | none | none |

Every pin above is claimed by exactly one peripheral or test point; this was
checked against the generated devicetree of both builds. The overlays disable
board defaults that would otherwise claim them: USART1 (PB6/PB7), I2C1
(PB8/PB9), I2C3 (PA8/PC9), SPI2 (PB12-PB15, N3) and the TIM2 PWM LED on PA5.
The on-board green LED LD2 shares PA5 with SPI1 SCK and can't be used.
