#ifndef ETHERNET_H
#define ETHERNET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ETHERNET_ADDRESS_LENGTH 6
#define ETHERNET_HEADER_LENGTH 14
#define ETHERNET_MAX_PAYLOAD 1500

#define ETHERNET_TYPE_IPV4 0x0800
#define ETHERNET_TYPE_ARP  0x0806

void ethernet_init(void);

bool ethernet_send(
    const uint8_t destination[ETHERNET_ADDRESS_LENGTH],
    uint16_t type,
    const void *payload,
    size_t length
);

void ethernet_receive(
    const uint8_t *frame,
    size_t length
);

#endif
