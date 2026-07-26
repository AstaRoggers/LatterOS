#ifndef ICMP_H
#define ICMP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void icmp_init(void);

void icmp_receive(
    uint32_t source,
    const uint8_t *packet,
    size_t length
);

bool icmp_ping(
    uint32_t destination,
    uint32_t timeout_milliseconds,
    uint32_t *elapsed_milliseconds
);

#endif
