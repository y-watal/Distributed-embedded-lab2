#include "status.h"

#include "app_state.h"
#include "blinkers.h"
#include "communication.h"
#include "current_sensing.h"
#include "motor_control.h"
#include "safety.h"

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>

#include <stdbool.h>
#include <stdint.h>

/* SOF, type, length, 36-byte payload, 8-bit additive checksum. */
#define STATUS_SOF 0xA5
#define STATUS_TYPE 0x03
#define STATUS_PAYLOAD_LENGTH 36
#define STATUS_FRAME_LENGTH (3 + STATUS_PAYLOAD_LENGTH + 1)
#define STATUS_PERIOD_MS 20
#define STATUS_STACK_SIZE 1024
#define STATUS_PRIORITY 5

static const struct device *const pi_uart = DEVICE_DT_GET(DT_NODELABEL(usart1));
K_SEM_DEFINE(status_due, 0, 1);
K_THREAD_STACK_DEFINE(status_stack, STATUS_STACK_SIZE);
static struct k_thread status_thread_data;
static uint16_t status_sequence;

static void status_tick(struct k_timer *timer)
{
    ARG_UNUSED(timer);
    k_sem_give(&status_due);
}

K_TIMER_DEFINE(status_timer, status_tick, NULL);

static void put_u16(uint8_t *payload, size_t offset, uint16_t value)
{
    payload[offset] = (uint8_t)value;
    payload[offset + 1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *payload, size_t offset, uint32_t value)
{
    for (int i = 0; i < 4; ++i) {
        payload[offset + i] = (uint8_t)(value >> (8 * i));
    }
}

static int16_t clamp_i16(int32_t value)
{
    if (value > INT16_MAX) {
        return INT16_MAX;
    }
    if (value < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)value;
}

static uint16_t clamp_u16(uint32_t value)
{
    return value > UINT16_MAX ? UINT16_MAX : (uint16_t)value;
}

static void send_status(void)
{
    uint8_t frame[STATUS_FRAME_LENGTH] = {STATUS_SOF, STATUS_TYPE,
                                          STATUS_PAYLOAD_LENGTH};
    uint8_t *payload = &frame[3];
    struct current_sensor_readings currents = {0};
    struct motor_telemetry motor = {0};
    struct command_counters counters = {0};
    struct wheel_command command;
    bool currents_valid = current_sensing_get_latest(&currents);
    bool command_valid = app_state_get_command(&command);

    motor_control_get_telemetry(&motor);
    communication_get_counters(&counters);

    put_u16(payload, 0, status_sequence++);
    payload[2] = (uint8_t)safety_get_state();
    payload[3] = (uint8_t)safety_get_fault();
    payload[4] = safety_get_state() == CAR_STATE_NORMAL
                 ? (uint8_t)blinkers_get_mode() : 3U; /* hazards */
    payload[5] = (currents_valid ? 1U : 0U) | (command_valid ? 2U : 0U);
    put_u16(payload, 6, (uint16_t)clamp_i16(currents.left_ma));
    put_u16(payload, 8, (uint16_t)clamp_i16(currents.right_ma));
    put_u16(payload, 10, (uint16_t)clamp_i16(currents.servo_ma));
    put_u16(payload, 12, (uint16_t)clamp_i16(motor.left_speed_mm_s));
    put_u16(payload, 14, (uint16_t)clamp_i16(motor.right_speed_mm_s));
    put_u16(payload, 16, motor.left_duty_permille);
    put_u16(payload, 18, motor.right_duty_permille);
    put_u32(payload, 20, command_valid ? command.packet : 0U);
    put_u32(payload, 24, counters.valid);
    put_u16(payload, 28, clamp_u16(counters.bad_header));
    put_u16(payload, 30, clamp_u16(counters.bad_checksum));
    put_u16(payload, 32, clamp_u16(counters.bad_range));
    put_u16(payload, 34, clamp_u16(counters.dropped_bytes));

    uint8_t checksum = 0;
    for (size_t i = 1; i < STATUS_FRAME_LENGTH - 1; ++i) {
        checksum = (uint8_t)(checksum + frame[i]);
    }
    frame[STATUS_FRAME_LENGTH - 1] = checksum;

    for (size_t i = 0; i < sizeof(frame); ++i) {
        uart_poll_out(pi_uart, frame[i]);
    }
}

static void status_thread(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    if (!device_is_ready(pi_uart)) {
        return;
    }

    k_timer_start(&status_timer, K_MSEC(STATUS_PERIOD_MS),
                  K_MSEC(STATUS_PERIOD_MS));
    while (1) {
        k_sem_take(&status_due, K_FOREVER);
        send_status();
    }
}

void status_start(void)
{
    k_thread_create(&status_thread_data, status_stack,
                    K_THREAD_STACK_SIZEOF(status_stack),
                    status_thread, NULL, NULL, NULL,
                    STATUS_PRIORITY, 0, K_NO_WAIT);
}
