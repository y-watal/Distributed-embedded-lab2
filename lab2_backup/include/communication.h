#ifndef COMMUNICATION_H
#define COMMUNICATION_H

#include <stdint.h>

struct command_counters {
    uint32_t valid;
    uint32_t bad_header;
    uint32_t bad_checksum;
    uint32_t bad_range;
    uint32_t dropped_bytes;
};

void communication_get_counters(struct command_counters *counters);

#endif
