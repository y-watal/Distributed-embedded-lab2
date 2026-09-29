# Pi-to-STM32 serial protocol

USART1, 115200 baud, 8N1. The Pi uses `/dev/ttyAMA3`; the STM32 uses PB7 RX
and PB6 TX. Both directions use `SOF (0xA5) | type | payload length | payload |
checksum`. The checksum is the 8-bit sum of every byte from type through the
last payload byte. Multibyte values are little-endian. Receivers discard an
invalid header, length, checksum, or field value and scan for the next SOF.

## Pi command (`type=0x02`, length 19)

| Payload offset | Type | Field |
|---:|---|---|
| 0 | u32 | Proxy packet counter |
| 4 | i32 | Raw steering |
| 8 | i32 | Raw throttle |
| 12 | i32 | Raw brake |
| 16, 17, 18 | u8 | Buttons 4, 5, 10 |

The Pi forwards a UDP update only when steering, throttle, brake, or one of
the three buttons differs from the last frame it sent (the packet counter is
ignored), with at least 10 ms between UART transmissions. While the controls
are unchanged it resends the latest state every 50 ms as a keepalive. A valid command refreshes the STM32 link
timestamp. An out-of-range command is rejected, brakes the motors, starts
hazards, and sets `BAD_RANGE`. The next valid frame permits recovery. If no
valid command arrives for 90 ms, safety enters `LINK_TIMEOUT`; this leaves
margin for the 100 ms checkoff fault response.

## STM32 status (`type=0x03`, length 36)

The STM32 starts a new status transmission every 20 ms using a periodic Zephyr
timer and a dedicated thread. The frame is 40 bytes (about 3.5 ms of UART
airtime at 115200 baud). Its fields are:

| Offset | Type | Field |
|---:|---|---|
| 0 | u16 | Status sequence, wrapping at 65535 |
| 2 | u8 | State: 0 ERROR, 1 NORMAL, 2 SELF_TEST |
| 3 | u8 | Fault: 0 NONE, 1 LINK_TIMEOUT, 2 BAD_RANGE, 3 SELF_TEST |
| 4 | u8 | Indicator: 0 OFF, 1 LEFT, 2 RIGHT, 3 HAZARDS |
| 5 | u8 | Flags: bit 0 current sample ready, bit 1 accepted command stored |
| 6, 8, 10 | i16 | Left motor, right motor, servo current in mA |
| 12, 14 | i16 | Last control-loop left and right speeds in mm/s |
| 16, 18 | u16 | Left and right PWM duty in permille (0–1000) |
| 20 | u32 | Last accepted proxy packet counter, or 0 if invalidated |
| 24 | u32 | Valid STM32 command count |
| 28, 30, 32, 34 | u16 | Bad header, checksum, range, and dropped-byte counts |

Currents and speeds saturate at the limits of signed 16-bit integers; error
counts saturate at 65535. The current task refreshes its sample approximately
every 20 ms. Wheel speeds come from the motor control loop while active and
are zeroed when the controller leaves PID mode. The Pi validates and prints
the latest frame every 250 ms and continues parsing after a disconnected link
returns. The Pi's printed `interval` is measured between consecutive valid
frames; inspect several periods with a scope to verify the ±2 ms requirement.

The motor current labels reflect the present wiring: left is ADC channel 8
(PB0), right is channel 4 (PA4), and servo is channel 11 (PC1).
