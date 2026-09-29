# Pin map

Board: NUCLEO-F401RE
Zephyr target: nucleo_f401re/stm32f401xe

Pin assignments will be recorded here before wiring peripherals.

## Debug outputs

| Pin | Arduino | Function |
|-----|---------|----------|
| PA6 | D12 | Toggles on each valid command frame received from the Pi (USART1) |
| PA7 | D11 | Toggles on each new motor duty cycle written (drive duty change or coast) |

SPI1 is disabled in `app.overlay` because the board file assigns PA6/PA7 to it.
