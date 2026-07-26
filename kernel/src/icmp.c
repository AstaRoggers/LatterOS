#include "icmp.h"

#include "ipv4.h"
#include "memory.h"
#include "network.h"
#include "timer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ICMP_TYPE_ECHO_REPLY   0
#define ICMP_TYPE_ECHO_REQUEST 8

#define ICMP_HEADER_LENGTH 8
#define ICMP_PAYLOAD_LENGTH 24

static uint16_t identifier;
static uint16_t sequence_number;

static volatile bool reply_received;
static volatile uint32_t expected_source;
static volatile uint16_t expected_identifier;
static volatile uint16_t expected_sequence;

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

static uint16_t checksum(
    const uint8_t *data,
    size_t length
)
{
    uint32_t sum = 0;

    while (length >= 2)
    {
        sum +=
            ((uint16_t)data[0] << 8) |
            (uint16_t)data[1];

        data += 2;
        length -= 2;
    }

    if (length != 0)
    {
        sum +=
            (uint16_t)data[0] << 8;
    }

    while (sum >> 16)
    {
        sum =
            (sum & 0xFFFFU) +
            (sum >> 16);
    }

    return (uint16_t)~sum;
}

void icmp_init(void)
{
    identifier = 0x4C4F;
    sequence_number = 0;

    reply_received = false;
    expected_source = 0;
    expected_identifier = 0;
    expected_sequence = 0;
}

void icmp_receive(
    uint32_t source,
    const uint8_t *packet,
    size_t length
)
{
    if (
        packet == NULL ||
        length < ICMP_HEADER_LENGTH ||
        checksum(packet, length) != 0
    )
    {
        return;
    }

    uint8_t type = packet[0];
    uint8_t code = packet[1];

    if (code != 0)
    {
        return;
    }

    uint16_t packet_identifier =
        read_be16(&packet[4]);

    uint16_t packet_sequence =
        read_be16(&packet[6]);

    if (type == ICMP_TYPE_ECHO_REQUEST)
    {
        if (length > 1480)
        {
            return;
        }

        uint8_t reply[1480];

        memcpy(
            reply,
            packet,
            length
        );

        reply[0] = ICMP_TYPE_ECHO_REPLY;
        reply[1] = 0;

        write_be16(&reply[2], 0);

        write_be16(
            &reply[2],
            checksum(reply, length)
        );

        (void)ipv4_send(
            source,
            IPV4_PROTOCOL_ICMP,
            reply,
            length
        );

        return;
    }

    if (
        type == ICMP_TYPE_ECHO_REPLY &&
        source == expected_source &&
        packet_identifier == expected_identifier &&
        packet_sequence == expected_sequence
    )
    {
        reply_received = true;
    }
}

bool icmp_ping(
    uint32_t destination,
    uint32_t timeout_milliseconds,
    uint32_t *elapsed_milliseconds
)
{
    uint8_t packet[
        ICMP_HEADER_LENGTH +
        ICMP_PAYLOAD_LENGTH
    ];

    sequence_number++;

    packet[0] = ICMP_TYPE_ECHO_REQUEST;
    packet[1] = 0;

    write_be16(&packet[2], 0);
    write_be16(&packet[4], identifier);
    write_be16(&packet[6], sequence_number);

    static const uint8_t payload[
        ICMP_PAYLOAD_LENGTH
    ] = {
        'L', 'a', 't', 't', 'e', 'r',
        'O', 'S', ' ', 'I', 'C', 'M',
        'P', ' ', 'e', 'c', 'h', 'o',
        ' ', 't', 'e', 's', 't', '!'
    };

    memcpy(
        &packet[ICMP_HEADER_LENGTH],
        payload,
        sizeof(payload)
    );

    write_be16(
        &packet[2],
        checksum(packet, sizeof(packet))
    );

    reply_received = false;
    expected_source = destination;
    expected_identifier = identifier;
    expected_sequence = sequence_number;

    uint64_t start = timer_ticks();

    if (
        !ipv4_send(
            destination,
            IPV4_PROTOCOL_ICMP,
            packet,
            sizeof(packet)
        )
    )
    {
        return false;
    }

    uint32_t frequency =
        timer_frequency();

    uint64_t timeout_ticks =
        (
            (uint64_t)timeout_milliseconds *
            frequency +
            999U
        ) / 1000U;

    if (timeout_ticks == 0)
    {
        timeout_ticks = 1;
    }

    while (
        !reply_received &&
        timer_ticks() - start <
            timeout_ticks
    )
    {
        network_poll();
        __asm__ volatile("pause");
    }

    network_poll();

    uint64_t elapsed_ticks =
        timer_ticks() - start;

    if (elapsed_milliseconds != NULL)
    {
        *elapsed_milliseconds =
            (uint32_t)(
                elapsed_ticks *
                1000U /
                frequency
            );
    }

    return reply_received;
}
