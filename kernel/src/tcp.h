#ifndef TCP_H
#define TCP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum
{
    TCP_STATE_CLOSED,
    TCP_STATE_SYN_SENT,
    TCP_STATE_ESTABLISHED,
    TCP_STATE_FIN_WAIT_1,
    TCP_STATE_FIN_WAIT_2,
    TCP_STATE_CLOSE_WAIT,
    TCP_STATE_LAST_ACK
} tcp_state_t;

void tcp_init(void);

bool tcp_connect(
    uint32_t destination_address,
    uint16_t destination_port,
    uint32_t timeout_ms
);

bool tcp_send_data(
    const void *data,
    size_t length,
    uint32_t timeout_ms
);

size_t tcp_receive_data(
    void *buffer,
    size_t capacity,
    uint32_t timeout_ms
);

void tcp_close(uint32_t timeout_ms);
void tcp_abort(void);

bool tcp_is_connected(void);
bool tcp_peer_closed(void);
tcp_state_t tcp_state(void);

void tcp_receive(
    uint32_t source_address,
    uint32_t destination_address,
    const uint8_t *packet,
    size_t length
);

uint64_t tcp_received_segments(void);
uint64_t tcp_transmitted_segments(void);
void tcp_print_status(void);

#endif
