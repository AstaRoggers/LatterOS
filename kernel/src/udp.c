#include "udp.h"

#include "ipv4.h"
#include "kstdio.h"
#include "memory.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UDP_HEADER_LENGTH 8
#define UDP_MAX_PAYLOAD   1472
#define UDP_BINDING_COUNT 16

typedef struct
{
    bool used;
    uint16_t port;
    udp_receive_handler_t handler;
} udp_binding_t;

static udp_binding_t bindings[UDP_BINDING_COUNT];
static uint64_t received_count;
static uint64_t transmitted_count;

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

void udp_init(void)
{
    for (
        uint8_t index = 0;
        index < UDP_BINDING_COUNT;
        index++
    )
    {
        bindings[index].used = false;
        bindings[index].port = 0;
        bindings[index].handler = NULL;
    }

    received_count = 0;
    transmitted_count = 0;
}

bool udp_bind(
    uint16_t port,
    udp_receive_handler_t handler
)
{
    if (
        port == 0 ||
        handler == NULL
    )
    {
        return false;
    }

    for (
        uint8_t index = 0;
        index < UDP_BINDING_COUNT;
        index++
    )
    {
        if (
            bindings[index].used &&
            bindings[index].port == port
        )
        {
            bindings[index].handler = handler;
            return true;
        }
    }

    for (
        uint8_t index = 0;
        index < UDP_BINDING_COUNT;
        index++
    )
    {
        if (!bindings[index].used)
        {
            bindings[index].used = true;
            bindings[index].port = port;
            bindings[index].handler = handler;
            return true;
        }
    }

    return false;
}

void udp_unbind(uint16_t port)
{
    for (
        uint8_t index = 0;
        index < UDP_BINDING_COUNT;
        index++
    )
    {
        if (
            bindings[index].used &&
            bindings[index].port == port
        )
        {
            bindings[index].used = false;
            bindings[index].port = 0;
            bindings[index].handler = NULL;
            return;
        }
    }
}

bool udp_send_from(
    uint32_t source_address,
    uint32_t destination_address,
    uint16_t source_port,
    uint16_t destination_port,
    const void *payload,
    size_t length
)
{
    if (
        source_port == 0 ||
        destination_port == 0 ||
        (payload == NULL && length != 0) ||
        length > UDP_MAX_PAYLOAD
    )
    {
        return false;
    }

    uint8_t packet[
        UDP_HEADER_LENGTH +
        UDP_MAX_PAYLOAD
    ];

    write_be16(
        &packet[0],
        source_port
    );

    write_be16(
        &packet[2],
        destination_port
    );

    write_be16(
        &packet[4],
        (uint16_t)(
            UDP_HEADER_LENGTH +
            length
        )
    );

    /* A zero UDP checksum is valid for IPv4. */
    write_be16(&packet[6], 0);

    if (length != 0)
    {
        memcpy(
            &packet[UDP_HEADER_LENGTH],
            payload,
            length
        );
    }

    if (
        !ipv4_send_from(
            source_address,
            destination_address,
            IPV4_PROTOCOL_UDP,
            packet,
            UDP_HEADER_LENGTH + length
        )
    )
    {
        return false;
    }

    transmitted_count++;
    return true;
}

bool udp_send(
    uint32_t destination_address,
    uint16_t source_port,
    uint16_t destination_port,
    const void *payload,
    size_t length
)
{
    return udp_send_from(
        ipv4_local_address(),
        destination_address,
        source_port,
        destination_port,
        payload,
        length
    );
}

void udp_receive(
    uint32_t source_address,
    uint32_t destination_address,
    const uint8_t *packet,
    size_t length
)
{
    (void)destination_address;

    if (
        packet == NULL ||
        length < UDP_HEADER_LENGTH
    )
    {
        return;
    }

    uint16_t source_port =
        read_be16(&packet[0]);

    uint16_t destination_port =
        read_be16(&packet[2]);

    uint16_t udp_length =
        read_be16(&packet[4]);

    if (
        source_port == 0 ||
        destination_port == 0 ||
        udp_length < UDP_HEADER_LENGTH ||
        udp_length > length
    )
    {
        return;
    }

    received_count++;

    const uint8_t *payload =
        &packet[UDP_HEADER_LENGTH];

    size_t payload_length =
        udp_length - UDP_HEADER_LENGTH;

    for (
        uint8_t index = 0;
        index < UDP_BINDING_COUNT;
        index++
    )
    {
        if (
            bindings[index].used &&
            bindings[index].port ==
                destination_port
        )
        {
            bindings[index].handler(
                source_address,
                source_port,
                destination_port,
                payload,
                payload_length
            );

            return;
        }
    }
}

uint64_t udp_received_datagrams(void)
{
    return received_count;
}

uint64_t udp_transmitted_datagrams(void)
{
    return transmitted_count;
}

uint32_t udp_bound_port_count(void)
{
    uint32_t count = 0;

    for (
        uint8_t index = 0;
        index < UDP_BINDING_COUNT;
        index++
    )
    {
        if (bindings[index].used)
        {
            count++;
        }
    }

    return count;
}

void udp_print_status(void)
{
    kprintf(
        "UDP: ready, bound=%u rx=%llu tx=%llu\n",
        udp_bound_port_count(),
        (unsigned long long)received_count,
        (unsigned long long)transmitted_count
    );
}
