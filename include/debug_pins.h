#ifndef DEBUG_PINS_H
#define DEBUG_PINS_H

// Timing debug outputs for a scope or logic analyzer
// PA6 (D12): toggles on every valid command frame received from the Pi
// PA7 (D11): toggles on every new motor duty cycle written to the PWM

void debug_pin_uart_command_toggle(void);
void debug_pin_motor_duty_toggle(void);

#endif
