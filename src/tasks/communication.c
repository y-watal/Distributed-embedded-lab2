#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

// Each received line can contain up to 191 characters plus a terminating zero
#define RX_LINE_SIZE 192

// Hold up to eight completed lines while the communication thread processes them
#define RX_QUEUE_DEPTH 8

// Memory reserved for the communication thread's local variables and function calls
#define COMM_STACK_SIZE 2048

// Nonnegative priorities are preemptive in Zephyr
// Lower numbers have higher priority
#define COMM_PRIORITY 4

// Get the USART1 device configured in app.overlay
// PB7 receives from the Pi and PB6 is available for transmitting back
static const struct device *const pi_uart = DEVICE_DT_GET(DT_NODELABEL(usart1));

// Thread-safe queue connecting the UART interrupt handler to the communication thread
// Each queue entry is a COPY of one fixed-size line buffer
// The final argument requests four-byte alignment for the queue storage
K_MSGQ_DEFINE(rx_lines, RX_LINE_SIZE, RX_QUEUE_DEPTH, 4);

// Only the UART interrupt handler modifies these variables
// They retain the partially received line between interrupts
static char rx_line[RX_LINE_SIZE];
static size_t rx_length;
static bool discard_line;

// Updated by the interrupt handler and read by the communication thread
// Atomic access allows both execution contexts to use this counter safely
static atomic_t dropped_lines;

// Values extracted from one of the Pi's wheel-state messages
// These remain raw values from the Pi and are not actuator commands yet
struct wheel_command {
    uint32_t packet;
    int steering;
    int throttle;
    int brake;
    int button4;
    int button5;
};

/*
 * Collect received UART bytes and queue complete newline-terminated lines
 *
 * Runs in INTERRUPT CONTEXT when the UART driver invokes the registered callback
 * A single interrupt can contain part of a line, a full line, or several lines
 *
 * MUTATES rx_line, rx_length, discard_line, dropped_lines, and rx_lines
 * MUST NOT sleep, wait for queue space, parse messages, or print debug output here
 */
static void uart_rx_callback(const struct device *dev, void *user_data)
{
    uint8_t byte;

    // The driver provides this argument but we do not need additional callback data
    (void)user_data;

    // Refresh the interrupt status before checking whether bytes are available
    uart_irq_update(dev);

    // Return if this interrupt has no received data to process
    if (!uart_irq_rx_ready(dev)) {
        return;
    }

    // Drain all currently available bytes from the UART receive buffer
    // Reading one byte at a time makes line assembly straightforward
    while (uart_fifo_read(dev, &byte, 1) == 1) {

        // A newline marks the end of the Pi's current message
        if (byte == '\n') {
            if (discard_line) {
                // Count the invalid or oversized line that we discarded
                atomic_inc(&dropped_lines);
            } else if (rx_length > 0) {
                // Terminate the line so the thread can treat it as a C string
                rx_line[rx_length] = '\0';

                // Copy the completed line into the queue without waiting
                // If the queue is full, drop this line and record the loss
                if (k_msgq_put(&rx_lines, rx_line, K_NO_WAIT) != 0) {
                    atomic_inc(&dropped_lines);
                }
            }

            // Prepare to collect the next line
            // Reusing rx_line is safe because the queue copied its contents
            rx_length = 0;
            discard_line = false;
            continue;
        }

        // Accept both LF and CRLF line endings by ignoring carriage returns
        // Once a line is rejected, ignore its remaining bytes until the newline
        if (byte == '\r' || discard_line) {
            continue;
        }

        // Reject embedded string terminators and lines that exceed the buffer
        // Reserve the final buffer position for the terminating zero
        if (byte == '\0' || rx_length >= sizeof(rx_line) - 1) {
            discard_line = true;
            continue;
        }

        // Append this byte and preserve it for the next iteration or interrupt
        rx_line[rx_length++] = (char)byte;
    }
}

/*
 * Extract wheel values from one completed line
 *
 * Runs in THREAD CONTEXT
 * MUTATES the command structure supplied by the caller
 * Returns true only when all six fields match and no trailing characters remain
 *
 * This checks the text format only
 * Calibrated value ranges and message integrity checks are not implemented yet
 */
static bool parse_command(const char *line, struct wheel_command *command)
{
    // Remains zero unless sscanf reaches the final %n conversion
    int consumed = 0;

    // Match the exact labels and separators produced by the Pi
    // SCNx32 reads the packet counter as a hexadecimal uint32_t
    // Each %d reads a decimal integer into the corresponding structure field
    // %n records the number of characters consumed and does not count as a field
    int fields = sscanf(
        line,
        "Receive state (Pkt: %" SCNx32 ") : Wheel: %d | Throttle: %d"
        " | Brake: %d | Button4: %d | Button5: %d%n",
        &command->packet,
        &command->steering,
        &command->throttle,
        &command->brake,
        &command->button4,
        &command->button5,
        &consumed
    );

    // The caller must ignore command if parsing failed because it may be partially filled
    return fields == 6 && consumed > 0 && line[consumed] == '\0';
}

/*
 * Initialize UART reception and process completed messages forever
 *
 * Runs as a NORMAL ZEPHYR THREAD with its own stack and priority
 * Sleeps while the queue is empty so other tasks can run
 * Parses and prints in thread context to keep the UART interrupt handler short
 *
 * The decoded command is currently local to this thread
 * Publishing commands to motor and steering tasks will be added later
 */
static void communication_thread(void *arg1, void *arg2, void *arg3)
{
    char line[RX_LINE_SIZE];
    struct wheel_command command;
    uint32_t received = 0;

    // Zephyr thread entry functions accept three arguments
    // This thread does not need any of them
    (void)arg1;
    (void)arg2;
    (void)arg3;

    // Check that Zephyr successfully initialized the USART1 driver
    if (!device_is_ready(pi_uart)) {
        printk("ERROR: USART1 is not ready\n");
        return;
    }

    // Register the function the UART driver will call during receive interrupts
    // NULL means we are not passing extra user data to the callback
    int ret = uart_irq_callback_user_data_set(pi_uart, uart_rx_callback, NULL);
    if (ret < 0) {
        printk("ERROR: UART callback setup failed: %d\n", ret);
        return;
    }

    // Allow incoming UART data to trigger receive interrupts
    uart_irq_rx_enable(pi_uart);

    printk("USART1 ready: PB7 RX, PB6 TX, 921600 baud\n");

    while (1) {
        // Wait for the next completed line and copy it into this thread's buffer
        // K_FOREVER blocks this thread until data arrives without busy-waiting
        k_msgq_get(&rx_lines, line, K_FOREVER);

        // The Pi also sends force-feedback debug lines over this UART
        // Those lines do not contain wheel commands
        if (strncmp(line, "Send force ", 11) == 0) {
            continue;
        }

        // Reject lines that do not match the expected wheel-state message
        if (!parse_command(line, &command)) {
            printk("Rejected UART line: %s\n", line);
            continue;
        }

        // Count successfully parsed messages separately from the Pi's packet counter
        received++;

        // Print decoded values through USART2 and ST-LINK USB to the Mac
        // These prints do not go back to the Pi on USART1
        printk(
            "RX %" PRIu32 " | pkt=%08" PRIx32
            " | steer=%d | throttle=%d | brake=%d"
            " | b4=%d | b5=%d | dropped=%ld\n",
            received,
            command.packet,
            command.steering,
            command.throttle,
            command.brake,
            command.button4,
            command.button5,
            (long)atomic_get(&dropped_lines)
        );
    }
}

/*
 * Create the communication thread and make it ready at startup
 *
 * Arguments specify the thread identifier, stack size, entry function,
 * three unused arguments, priority, options, and startup delay
 *
 * The final zero means no startup delay
 * Zephyr schedules this thread automatically so main does not call it
 */
K_THREAD_DEFINE(communication_tid, COMM_STACK_SIZE, communication_thread,
                NULL, NULL, NULL, COMM_PRIORITY, 0, 0);
