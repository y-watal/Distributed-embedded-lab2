#include "app_state.h"
#include "communication.h"
#include "safety.h"

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// Pi frame:
// SOF | type | length | 19-byte payload | checksum
//
// Payload:
// packet[4], steering[4], throttle[4], brake[4],
// button4[1], button5[1], button10[1]
//
// All multibyte values arrive little-endian
// Checksum is the 8-bit sum of type, length, and payload

#define UART_SOF          0xA5
#define MSG_STATE         0x02
#define STATE_LENGTH      19
#define FRAME_LENGTH      (3 + STATE_LENGTH + 1)

#define RX_QUEUE_DEPTH    256
#define COMM_STACK_SIZE   2048
#define COMM_PRIORITY     4
#define PRINT_INTERVAL_MS 100
#define DEBUG_OUTPUT_ENABLED 0

// The Pi sends commands into USART1 RX on PB7
static const struct device *const pi_uart = DEVICE_DT_GET(DT_NODELABEL(usart1));

// The ISR deposits bytes here without waiting
// The communication thread removes and parses them
K_MSGQ_DEFINE(rx_bytes, sizeof(uint8_t), RX_QUEUE_DEPTH, 1);

static atomic_t dropped_bytes;
static atomic_t valid_states;
static atomic_t bad_headers;
static atomic_t bad_checksums;
static atomic_t bad_ranges;

void communication_get_counters(struct command_counters *counters)
{
    if (counters == NULL) {
        return;
    }
    counters->valid = (uint32_t)atomic_get(&valid_states);
    counters->bad_header = (uint32_t)atomic_get(&bad_headers);
    counters->bad_checksum = (uint32_t)atomic_get(&bad_checksums);
    counters->bad_range = (uint32_t)atomic_get(&bad_ranges);
    counters->dropped_bytes = (uint32_t)atomic_get(&dropped_bytes);
}

// Only the communication thread accesses this partial frame
static uint8_t frame[FRAME_LENGTH];
static size_t frame_position;

static uint32_t read_u32_le(const uint8_t *bytes)
{
    return (uint32_t)bytes[0]
         | ((uint32_t)bytes[1] << 8)
         | ((uint32_t)bytes[2] << 16)
         | ((uint32_t)bytes[3] << 24);
}

static int32_t read_i32_le(const uint8_t *bytes)
{
    uint32_t bits = read_u32_le(bytes);
    int32_t value;

    // Copy the signed bit pattern without depending on a packed struct
    memcpy(&value, &bits, sizeof(value));

    return value;
}

static bool command_in_range(const struct wheel_command *command)
{
    if (command->steering < -32767 || command->steering > 32766) {
        return false;
    }

    if (command->throttle < 0 || command->throttle > 32767) {
        return false;
    }

    if (command->brake < 0 || command->brake > 32767) {
        return false;
    }

    if ((command->button4 != 0 && command->button4 != 128) ||
        (command->button5 != 0 && command->button5 != 128) ||
        (command->button10 != 0 && command->button10 != 128)) {
        return false;
    }

    return true;
}

static uint8_t frame_checksum(const uint8_t *bytes)
{
    uint8_t sum = 0;

    // Exclude SOF and the final checksum byte
    for (size_t i = 1; i < FRAME_LENGTH - 1; i++) {
        sum = (uint8_t)(sum + bytes[i]);
    }

    return sum;
}

static void decode_state(struct wheel_command *command)
{
    const uint8_t *payload = &frame[3];

    command->packet = read_u32_le(&payload[0]);
    command->steering = read_i32_le(&payload[4]);
    command->throttle = read_i32_le(&payload[8]);
    command->brake = read_i32_le(&payload[12]);
    command->button4 = payload[16];
    command->button5 = payload[17];
    command->button10 = payload[18];
    command->received_at_ms = 0;
}

// Feed one received byte into the frame parser
// This runs in the communication thread, not in the UART interrupt
static void parse_byte(uint8_t byte)
{
    if (frame_position == 0) {
        if (byte == UART_SOF) {
            frame[frame_position++] = byte;
        } else {
            atomic_inc(&bad_headers);
        }
        return;
    }

    if (frame_position == 1) {
        if (byte != MSG_STATE) {
            atomic_inc(&bad_headers);
            frame_position = 0;

            // This byte might itself be the start of a new frame
            if (byte == UART_SOF) {
                frame[frame_position++] = byte;
            }
            return;
        }

        frame[frame_position++] = byte;
        return;
    }

    if (frame_position == 2) {
        if (byte != STATE_LENGTH) {
            atomic_inc(&bad_headers);
            frame_position = 0;

            if (byte == UART_SOF) {
                frame[frame_position++] = byte;
            }
            return;
        }

        frame[frame_position++] = byte;
        return;
    }

    frame[frame_position++] = byte;

    if (frame_position < FRAME_LENGTH) {
        return;
    }

    // A complete frame has arrived
    frame_position = 0;

    if (frame_checksum(frame) != frame[FRAME_LENGTH - 1]) {
        atomic_inc(&bad_checksums);
        return;
    }

    struct wheel_command command;
    decode_state(&command);

    if (!command_in_range(&command)) {
        atomic_inc(&bad_ranges);
        safety_bad_range();
        return;
    }

    // The only path that updates the shared command or its timestamp
    app_state_set_command(&command);
    atomic_inc(&valid_states);
}

// UART interrupt handler
// It only drains hardware bytes into the queue
// It does not parse, print, lock a mutex, or wait
static void uart_rx_callback(const struct device *dev, void *user_data)
{
    uint8_t byte;

    (void)user_data;

    uart_irq_update(dev);

    if (!uart_irq_rx_ready(dev)) {
        return;
    }

    while (uart_fifo_read(dev, &byte, 1) == 1) {
        if (k_msgq_put(&rx_bytes, &byte, K_NO_WAIT) != 0) {
            atomic_inc(&dropped_bytes);
        }
    }
}

static void print_debug_state(void)
{
    struct wheel_command command;

    if (!app_state_get_command(&command)) {
        printk("waiting for valid Pi command | states=%u bad_hdr=%u "
               "bad_sum=%u bad_range=%u dropped=%ld\n",
               (unsigned int)atomic_get(&valid_states),
               (unsigned int)atomic_get(&bad_headers),
               (unsigned int)atomic_get(&bad_checksums),
               (unsigned int)atomic_get(&bad_ranges),
               (long)atomic_get(&dropped_bytes));
        return;
    }

/*
    int64_t age_ms = k_uptime_get() - command.received_at_ms;
    printk("pkt=%u steer=%d throttle=%d brake=%d "
           "b4=%u b5=%u b10=%u | age=%ldms | "
           "states=%u bad_hdr=%u bad_sum=%u bad_range=%u dropped=%ld\n",
           command.packet,
           command.steering,
           command.throttle,
           command.brake,
           command.button4,
           command.button5,
           command.button10,
           (long)age_ms,
           (unsigned int)atomic_get(&valid_states),
           (unsigned int)atomic_get(&bad_headers),
           (unsigned int)atomic_get(&bad_checksums),
           (unsigned int)atomic_get(&bad_ranges),
           (long)atomic_get(&dropped_bytes));
           */
}

static void communication_thread(void *arg1, void *arg2, void *arg3)
{
    uint8_t byte;
    int64_t last_print_ms = 0;

    (void)arg1;
    (void)arg2;
    (void)arg3;

    if (!device_is_ready(pi_uart)) {
        printk("ERROR: USART1 is not ready\n");
        return;
    }

    int ret = uart_irq_callback_user_data_set(
        pi_uart, uart_rx_callback, NULL
    );

    if (ret < 0) {
        printk("ERROR: UART callback setup failed: %d\n", ret);
        return;
    }

    uart_irq_rx_enable(pi_uart);
    printk("USART1 ready: PB7 RX, PB6 TX, 115200 baud\n");

    while (1) {
        // Wake for received bytes or periodically to keep debug output visible
        if (k_msgq_get(&rx_bytes, &byte, K_MSEC(20)) == 0) {
            parse_byte(byte);
        }

        int64_t now_ms = k_uptime_get();

        if (DEBUG_OUTPUT_ENABLED &&
            now_ms - last_print_ms >= PRINT_INTERVAL_MS) {
            last_print_ms = now_ms;
            print_debug_state();
        }
    }
}

K_THREAD_DEFINE(communication_tid, COMM_STACK_SIZE, communication_thread,
                NULL, NULL, NULL, COMM_PRIORITY, 0, 0);
