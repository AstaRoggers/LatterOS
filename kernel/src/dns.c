#include "dns.h"

#include "ipv4.h"
#include "kstdio.h"
#include "kstring.h"
#include "memory.h"
#include "network.h"
#include "timer.h"
#include "udp.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DNS_SERVER_PORT 53
#define DNS_CLIENT_PORT 53000
#define DNS_PACKET_SIZE 512

#define DNS_TYPE_A    1
#define DNS_CLASS_IN  1

static uint32_t configured_server;
static uint16_t next_transaction;

static volatile bool query_waiting;
static volatile bool query_received;
static volatile bool query_failed;
static uint16_t expected_transaction;
static uint32_t resolved_address;

static uint16_t read_be16(
    const uint8_t *data
)
{
    return
        ((uint16_t)data[0] << 8) |
        (uint16_t)data[1];
}

static uint32_t read_be32(
    const uint8_t *data
)
{
    return
        ((uint32_t)data[0] << 24) |
        ((uint32_t)data[1] << 16) |
        ((uint32_t)data[2] << 8) |
        (uint32_t)data[3];
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

static size_t skip_name(
    const uint8_t *packet,
    size_t length,
    size_t offset
)
{
    while (offset < length)
    {
        uint8_t label = packet[offset];

        if (label == 0)
        {
            return offset + 1;
        }

        if ((label & 0xC0U) == 0xC0U)
        {
            if (offset + 1 >= length)
            {
                return 0;
            }

            return offset + 2;
        }

        if (
            label > 63 ||
            offset + 1U + label > length
        )
        {
            return 0;
        }

        offset += 1U + label;
    }

    return 0;
}

static void dns_receive(
    uint32_t source_address,
    uint16_t source_port,
    uint16_t destination_port,
    const uint8_t *packet,
    size_t length
)
{
    (void)destination_port;

    if (
        !query_waiting ||
        source_address != configured_server ||
        source_port != DNS_SERVER_PORT ||
        packet == NULL ||
        length < 12 ||
        read_be16(&packet[0]) !=
            expected_transaction
    )
    {
        return;
    }

    uint16_t flags = read_be16(&packet[2]);

    if (
        !(flags & 0x8000U) ||
        (flags & 0x000FU) != 0
    )
    {
        query_failed = true;
        query_waiting = false;
        return;
    }

    uint16_t question_count =
        read_be16(&packet[4]);

    uint16_t answer_count =
        read_be16(&packet[6]);

    size_t offset = 12;

    for (
        uint16_t question = 0;
        question < question_count;
        question++
    )
    {
        offset = skip_name(
            packet,
            length,
            offset
        );

        if (
            offset == 0 ||
            offset + 4 > length
        )
        {
            query_failed = true;
            query_waiting = false;
            return;
        }

        offset += 4;
    }

    for (
        uint16_t answer = 0;
        answer < answer_count;
        answer++
    )
    {
        offset = skip_name(
            packet,
            length,
            offset
        );

        if (
            offset == 0 ||
            offset + 10 > length
        )
        {
            query_failed = true;
            query_waiting = false;
            return;
        }

        uint16_t type =
            read_be16(&packet[offset]);

        uint16_t class_code =
            read_be16(&packet[offset + 2]);

        uint16_t data_length =
            read_be16(&packet[offset + 8]);

        offset += 10;

        if (offset + data_length > length)
        {
            query_failed = true;
            query_waiting = false;
            return;
        }

        if (
            type == DNS_TYPE_A &&
            class_code == DNS_CLASS_IN &&
            data_length == 4
        )
        {
            resolved_address =
                read_be32(&packet[offset]);

            query_received = true;
            query_waiting = false;
            return;
        }

        offset += data_length;
    }

    query_failed = true;
    query_waiting = false;
}

static size_t encode_name(
    const char *name,
    uint8_t *output,
    size_t capacity
)
{
    if (
        name == NULL ||
        output == NULL ||
        capacity < 2
    )
    {
        return 0;
    }

    size_t name_length = kstrlen(name);

    if (
        name_length == 0 ||
        name_length > 253
    )
    {
        return 0;
    }

    size_t input = 0;
    size_t output_offset = 0;

    while (input < name_length)
    {
        size_t label_start = input;

        while (
            input < name_length &&
            name[input] != '.'
        )
        {
            input++;
        }

        size_t label_length =
            input - label_start;

        if (
            label_length == 0 ||
            label_length > 63 ||
            output_offset + 1U +
                label_length >= capacity
        )
        {
            return 0;
        }

        output[output_offset++] =
            (uint8_t)label_length;

        memcpy(
            &output[output_offset],
            &name[label_start],
            label_length
        );

        output_offset += label_length;

        if (
            input < name_length &&
            name[input] == '.'
        )
        {
            input++;

            if (input == name_length)
            {
                break;
            }
        }
    }

    if (output_offset >= capacity)
    {
        return 0;
    }

    output[output_offset++] = 0;
    return output_offset;
}

void dns_init(uint32_t server_address)
{
    configured_server = server_address;
    next_transaction =
        (uint16_t)(0x4C00U ^ timer_ticks());

    query_waiting = false;
    query_received = false;
    query_failed = false;
    expected_transaction = 0;
    resolved_address = 0;

    (void)udp_bind(
        DNS_CLIENT_PORT,
        dns_receive
    );
}

void dns_set_server(uint32_t server_address)
{
    if (server_address != 0)
    {
        configured_server = server_address;
    }
}

uint32_t dns_server(void)
{
    return configured_server;
}

bool dns_resolve_a(
    const char *name,
    uint32_t timeout_iterations,
    uint32_t *address
)
{
    if (
        name == NULL ||
        address == NULL ||
        configured_server == 0 ||
        !network_is_ready()
    )
    {
        return false;
    }

    uint8_t packet[DNS_PACKET_SIZE];
    memset(packet, 0, sizeof(packet));

    next_transaction++;

    if (next_transaction == 0)
    {
        next_transaction = 1;
    }

    write_be16(
        &packet[0],
        next_transaction
    );

    write_be16(&packet[2], 0x0100);
    write_be16(&packet[4], 1);

    size_t name_length =
        encode_name(
            name,
            &packet[12],
            sizeof(packet) - 12
        );

    if (
        name_length == 0 ||
        12U + name_length + 4U >
            sizeof(packet)
    )
    {
        return false;
    }

    size_t length = 12U + name_length;

    write_be16(
        &packet[length],
        DNS_TYPE_A
    );

    write_be16(
        &packet[length + 2],
        DNS_CLASS_IN
    );

    length += 4;

    query_waiting = true;
    query_received = false;
    query_failed = false;
    expected_transaction =
        next_transaction;

    if (
        !udp_send(
            configured_server,
            DNS_CLIENT_PORT,
            DNS_SERVER_PORT,
            packet,
            length
        )
    )
    {
        query_waiting = false;
        return false;
    }

    if (timeout_iterations == 0)
    {
        timeout_iterations = 5000000U;
    }

    for (
        uint32_t iteration = 0;
        iteration < timeout_iterations;
        iteration++
    )
    {
        network_poll();

        if (
            query_received ||
            query_failed
        )
        {
            break;
        }

        __asm__ volatile("pause");
    }

    query_waiting = false;

    if (!query_received)
    {
        return false;
    }

    *address = resolved_address;
    return true;
}

void dns_print_server(void)
{
    char text[16];

    ipv4_format_address(
        configured_server,
        text,
        sizeof(text)
    );

    kprintf("DNS server: %s\n", text);
}
