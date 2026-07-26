#include "ethernet.h"

#include "arp.h"
#include "ipv4.h"
#include "memory.h"
#include "network.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static uint16_t read_be16(
    const uint8_t *data
)
{
    return
        ((uint16_t)data[0] << 8) |
        (uint16_t)data[1];
}

static void write_be16(
    uint8_t *data,
    uint16_t value
)
{
    data[0] =
        (uint8_t)(value >> 8);

    data[1] =
        (uint8_t)value;
}

void ethernet_init(void)
{
    network_set_receive_handler(
        ethernet_receive
    );
}

bool ethernet_send(
    const uint8_t destination[ETHERNET_ADDRESS_LENGTH],
    uint16_t type,
    const void *payload,
    size_t length
)
{
    if (
        destination == NULL ||
        (payload == NULL && length != 0) ||
        length > ETHERNET_MAX_PAYLOAD
    )
    {
        return false;
    }

    uint8_t frame[
        ETHERNET_HEADER_LENGTH +
        ETHERNET_MAX_PAYLOAD
    ];

    const uint8_t *source =
        network_mac_address();

    memcpy(
        &frame[0],
        destination,
        ETHERNET_ADDRESS_LENGTH
    );

    memcpy(
        &frame[6],
        source,
        ETHERNET_ADDRESS_LENGTH
    );

    write_be16(
        &frame[12],
        type
    );

    if (length != 0)
    {
        memcpy(
            &frame[ETHERNET_HEADER_LENGTH],
            payload,
            length
        );
    }

    return network_send_frame(
        frame,
        ETHERNET_HEADER_LENGTH + length
    );
}

void ethernet_receive(
    const uint8_t *frame,
    size_t length
)
{
    if (
        frame == NULL ||
        length < ETHERNET_HEADER_LENGTH
    )
    {
        return;
    }

    uint16_t type =
        read_be16(&frame[12]);

    const uint8_t *payload =
        &frame[ETHERNET_HEADER_LENGTH];

    size_t payload_length =
        length - ETHERNET_HEADER_LENGTH;

    if (type == ETHERNET_TYPE_ARP)
    {
        arp_receive(
            payload,
            payload_length
        );
    }
    else if (type == ETHERNET_TYPE_IPV4)
    {
        ipv4_receive(
            payload,
            payload_length
        );
    }
}
