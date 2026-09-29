#ifndef SAFETY_H
#define SAFETY_H

enum car_state {
    CAR_STATE_ERROR,
    CAR_STATE_NORMAL,
    CAR_STATE_SELF_TEST
};

enum fault_reason {
    FAULT_NONE,
    FAULT_LINK_TIMEOUT,
    FAULT_BAD_RANGE,
    FAULT_SELF_TEST
};

void safety_start(void);
enum car_state safety_get_state(void);
enum fault_reason safety_get_fault(void);
void safety_bad_range(void);

void safety_request_self_test_enter(void);
void safety_request_self_test_exit(void);

#endif
