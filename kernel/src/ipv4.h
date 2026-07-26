#ifndef IPV4_H
#define IPV4_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IPV4_ADDRESS(a, b, c, d) \
    (((uint32_t)(a) << 24) | \
     ((uint32_t)(b) << 16) | \
     ((uint32_t)(c) << 8) | \
     (uint32_t)(d))

#define IPV4_PROTOCOL_ICMP 1
#define IPV4_PROTOCOL_TCP  6
#define IPV4_PROTOCOL_UDP  17

void ipv4_init(
    uint32_t local_address,
    uint32_t netmask,
    uint32_t gateway
);

uint32_t ipv4_local_address(void);
uint32_t ipv4_netmask(void);
uint32_t ipv4_gateway(void);

bool ipv4_parse_address(
    const char *text,
    uint32_t *address
);

void ipv4_format_address(
    uint32_t address,
    char *buffer,
    size_t capacity
);

bool ipv4_send_from(
    uint32_t source,
    uint32_t destination,
    uint8_t protocol,
    const void *payload,
    size_t length
);

bool ipv4_send(
    uint32_t destination,
    uint8_t protocol,
    const void *payload,
    size_t length
);

void ipv4_receive(
    const uint8_t *packet,
    size_t length
);

void ipv4_print_config(void);

#endif
