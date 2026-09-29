# Task table

Zephyr runs the ready thread with the lowest priority number first. Every application thread below is preemptible (priority >= 0), so a higher-priority thread that becomes ready preempts a lower one right away. Threads at the same priority are time-sliced: `CONFIG_TIMESLICING=y` with a 20 ms slice (`CONFIG_TIMESLICE_SIZE=20`, `CONFIG_TIMESLICE_PRIORITY=0`).

Each priority, period, and stack size is a `#define` at the top of that task's source file.

## Application threads

Listed from highest priority to lowest.

| Prio | Task | Source | Activation | Period / trigger | Stack (B) | Responsibility |
|---|---|---|---|---|---|---|
| 0 | main | `src/main.c` | Runs once at boot | One time | 2048 | Initializes the motor, LEDs, current sensors (calibration), servo, and encoders. Starts the hazards, then starts the tasks below and returns. |
| 1 | Motor control | `src/tasks/motor_control.c` | Waits on `command_available` semaphore, 20 ms timeout | Wakes as soon as a new command is accepted, otherwise every 20 ms. The PID only runs once at least 20 ms have passed since the last encoder sample. | 1536 | Brakes on stale commands (90 ms or older) or when brake >= 500. Coasts inside the throttle deadband. Otherwise runs feed-forward + PI speed control per wheel from the encoder velocity. |
| 2 | Safety | `src/tasks/safety.c` | Periodic `k_sleep` | 5 ms | 1024 | Checks command freshness (under 90 ms). Handles ERROR / NORMAL / SELF_TEST transitions and motor drive permission. Applies the selected blinker pattern to the LEDs. |
| 3 | Blinkers | `src/tasks/blinkers.c` | Periodic `k_sleep` | 5 ms | 1024 | Detects button presses. Handles turn-signal selection and self-cancel (arms at 5000, cancels at 4000). Handles self-test entry (single press) and exit (double press within 350 ms), with a 30 ms debounce. |
| 4 | Communication | `src/tasks/communication.c` | Blocks on `rx_bytes` message queue, 20 ms timeout | Event-driven, runs once per received byte | 2048 | Parses Pi UART frames, checks the checksum and value ranges, and publishes valid commands. Created with `K_THREAD_DEFINE`, so it starts at boot, independent of `main()`. |
| 5 | Status TX | `src/tasks/status.c` | Blocks on `status_due` semaphore, which `status_timer` gives | 20 ms, timer-driven with no drift | 1024 | Builds a 40-byte status frame and sends it to the Pi on USART1 with `uart_poll_out`. |
| 5 | Steering | `src/tasks/steering.c` | Periodic `k_sleep` | 20 ms | 1024 | Maps steering input (clamped to +/-10000) to a servo angle from 0 to 180 degrees. Centers the servo when the state isn't NORMAL. Only writes to the servo when the angle changes. |
| 6 | Current sensing | `src/tasks/current_sensing.c` | Periodic `k_sleep` | 20 ms plus the ADC read time | 1024 | Reads the left motor, right motor, and servo current sensors and publishes the latest readings. Prints readings every 250 ms. |
| 6 | Motor debug | `src/tasks/motor_control.c` | Periodic `k_sleep` | 250 ms | 1024 | Prints controller telemetry. Only created when `DEBUG_OUTPUT_ENABLED` is set (currently 0). |

## Interrupt and kernel-context work

| Priority | Context | Source | Trigger | Work |
|---|---|---|---|---|
| IRQ 0 (DT `interrupts = <37 0>`) | USART1 RX interrupt | `uart_rx_callback`, `src/tasks/communication.c` | Each received byte | Copies bytes from the hardware FIFO into `rx_bytes` without waiting, and counts any bytes it drops. Does no parsing. |
| IRQ 0 (`CONFIG_CORTEX_M_SYSTICK_INTERRUPT_PRIORITY=0`) | SysTick interrupt, timer expiry | `status_tick`, `src/tasks/status.c` | Every 20 ms | Gives `status_due`. |
| Thread -1, cooperative (`CONFIG_SYSTEM_WORKQUEUE_PRIORITY=-1`) | System workqueue | `blink_work`, `src/drivers/led.c` | Delayable work that reschedules itself | Toggles the blinker phase every 250 ms for hazards (2 Hz) and every 500 ms for turn signals (1 Hz). |
| None (no interrupt) | Hardware | TIM1 and TIM2, `src/drivers/encoder.c` | Quadrature encoder mode | Counts encoder edges with no interrupts. Motor control reads the counters when it runs. |

## Priority order

Everything that can run on the MCU, from the highest priority to the lowest.
Both interrupts outrank every thread. The cooperative workqueue can only be
interrupted by an ISR, never by a thread. Preemptible threads are preempted by
anything above them.

| Rank | Priority | Context | Deadline / rate | Why it has this priority |
|---|---|---|---|---|
| 1 | IRQ 0 | SysTick | 10 kHz tick (`CONFIG_SYS_CLOCK_TICKS_PER_SEC=10000`) | Drives every kernel timeout, `k_sleep`, and the 20 ms status timer. |
| 1 | IRQ 0 | USART1 RX | One byte every ~87 us at 115200 baud | Must drain the RX FIFO before it overruns. The handler only queues bytes, so it stays short. Same NVIC level as SysTick, so neither preempts the other. |
| 2 | -1 (coop) | System workqueue: blink work | Blink edge every 250 ms (hazard) or 500 ms (turn) | Runs to completion, so all four LEDs toggle together within one handler (1 ms front/rear skew requirement). |
| 3 | 0 | main | One time at boot | Kernel default. Returns after initialization and starting the tasks. |
| 4 | 1 | Motor control | Brake at most 2 ms end to end; PID every 20 ms | Safety-critical actuator. Highest application priority so a new brake command preempts every other application thread as soon as it's published. |
| 5 | 2 | Safety | 5 ms poll; link loss detected within ~95 ms | Owns the car state and drive permission. Revokes drive on link loss or self-test. Only the actuator loop, which checks the state itself, is above it. |
| 6 | 3 | Blinkers | 5 ms poll | Button edges, turn-signal self-cancel, and self-test requests that feed safety. |
| 7 | 4 | Communication | Event-driven per byte | Parsing is short per byte and the 256-byte queue absorbs bursts, so it sits below the control tasks and can't delay them. It still outranks the 20 ms tasks so commands are parsed promptly. |
| 8 | 5 | Status TX | 20 ms, timer-driven | Telemetry to the Pi. No hard deadline. |
| 8 | 5 | Steering | 20 ms poll; 50 ms steering deadline | 50 ms deadline is loose compared with the tasks above. Time-sliced with status TX. |
| 9 | 6 | Current sensing | 20 ms poll | Monitoring and logging only. No deadline in lab 2. |
| 9 | 6 | Motor debug (disabled) | 250 ms | Debug printing only. |
| 10 | 15 | Idle | Whenever nothing else is ready | Kernel idle thread (`CONFIG_NUM_PREEMPT_PRIORITIES=15`). |

## Shared data and synchronization

| Data | Writer | Readers | Protection |
|---|---|---|---|
| Latest wheel command (`app_state.c`) | Communication | Motor control, safety, blinkers, steering, status | `command_mutex`. The `command_available` semaphore (max count 1) wakes motor control. |
| Car state and fault reason | Safety, and communication through `safety_bad_range()` | All tasks | `atomic_t` |
| Selected indicator | Blinkers | Safety, status | `atomic_t` |
| Current readings | Current sensing | Status | `current_readings_mutex` |
| Motor telemetry | Motor control | Status | `atomic_t` |
| UART counters | Communication | Status | `atomic_t` |

## Key timing paths

| Path | Chain | Expected worst case |
|---|---|---|
| Brake | RX interrupt -> communication (4) -> semaphore -> motor control (1) preempts -> H-bridge brake | Well under 2 ms after the last byte of the frame |
| Throttle | Same chain as brake, then the PID waits for its 20 ms sample interval | Up to about 20 ms for a new duty |
| Link loss -> ERROR | Command goes stale (90 ms), then safety notices on its next 5 ms poll | About 95 ms |
| Turn-signal self-cancel | Blinkers 5 ms poll -> safety 5 ms poll -> LED update | About 10 ms |
| Self-test press -> brake and hazards | Communication -> blinkers 5 ms poll -> safety 5 ms poll | About 10 ms after the frame arrives |

## Notes

- Status TX and steering share priority 5. Status TX busy-waits in
  `uart_poll_out` for about 3.5 ms per frame (40 bytes at 115200 baud). That
  is shorter than the 20 ms time slice, so steering and current sensing can't
  run until the whole frame has been sent.
- The design document's rates are faster than these periods: the drivetrain
  PID loop every 5 ms and servo current sampling every 1 ms. Here both run
  every 20 ms.
