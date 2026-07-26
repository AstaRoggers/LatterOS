#ifndef UDP_H
#define UDP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef void (*udp_receive_handler_t)(
    uint32_t source_address,
    uint16_t source_port,
    uint16_t destination_port,
    const uint8_t *payload,
    size_t length
);

void udp_init(void);

bool udp_bind(
    uint16_t port,
    udp_receive_handler_t handler
);

void udp_unbind(uint16_t port);

bool udp_send(
    uint32_t destination_address,
    uint16_t source_port,
    uint16_t destination_port,
    const void *payload,
    size_t length
);

bool udp_send_from(
    uint32_t source_address,
    uint32_t destination_address,
    uint16_t source_port,
    uint16_t destination_port,
    const void *payload,
    size_t length
);

void udp_receive(
    uint32_t source_address,
    uint32_t destination_address,
    const uint8_t *packet,
    size_t length
);

uint64_t udp_received_datagrams(void);
uint64_t udp_transmitted_datagrams(void);
uint32_t udp_bound_port_count(void);

void udp_print_status(void);

#endif
