#ifndef NETWORK_H
#define NETWORK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef void (*network_receive_handler_t)(
    const uint8_t *frame,
    size_t length
);

bool network_init(void);
bool network_is_ready(void);
bool network_link_up(void);

const uint8_t *network_mac_address(void);

void network_set_receive_handler(
    network_receive_handler_t handler
);

void network_poll(void);

bool network_send_frame(
    const void *frame,
    size_t length
);

bool network_send_test_frame(void);

uint64_t network_received_frames(void);
uint64_t network_transmitted_frames(void);

void network_print_status(void);

#endif
