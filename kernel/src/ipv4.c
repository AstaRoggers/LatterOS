#include "ipv4.h"

#include "arp.h"
#include "ethernet.h"
#include "icmp.h"
#include "kstdio.h"
#include "memory.h"
#include "tcp.h"
#include "udp.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IPV4_HEADER_MIN_LENGTH 20
#define IPV4_MAX_PACKET_LENGTH 1500

typedef struct __attribute__((packed))
{
    uint8_t version_ihl;
    uint8_t dscp_ecn;
    uint8_t total_length[2];
    uint8_t identification[2];
    uint8_t flags_fragment[2];
    uint8_t ttl;
    uint8_t protocol;
    uint8_t checksum[2];
    uint8_t source[4];
    uint8_t destination[4];
} ipv4_header_t;

static uint32_t local_ip;
static uint32_t local_mask;
static uint32_t default_gateway;
static uint16_t next_identification;

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

static uint32_t read_ipv4(
    const uint8_t *data
)
{
    return
        ((uint32_t)data[0] << 24) |
        ((uint32_t)data[1] << 16) |
        ((uint32_t)data[2] << 8) |
        (uint32_t)data[3];
}

static void write_ipv4(
    uint8_t *data,
    uint32_t address
)
{
    data[0] =
        (uint8_t)(address >> 24);

    data[1] =
        (uint8_t)(address >> 16);

    data[2] =
        (uint8_t)(address >> 8);

    data[3] =
        (uint8_t)address;
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

void ipv4_init(
    uint32_t local_address,
    uint32_t netmask,
    uint32_t gateway
)
{
    local_ip = local_address;
    local_mask = netmask;
    default_gateway = gateway;
    next_identification = 1;

    arp_init(local_ip);
}

uint32_t ipv4_local_address(void)
{
    return local_ip;
}

uint32_t ipv4_netmask(void)
{
    return local_mask;
}

uint32_t ipv4_gateway(void)
{
    return default_gateway;
}

bool ipv4_parse_address(
    const char *text,
    uint32_t *address
)
{
    if (
        text == NULL ||
        address == NULL
    )
    {
        return false;
    }

    uint32_t octets[4] = {0, 0, 0, 0};
    uint8_t octet = 0;
    bool has_digit = false;

    for (
        size_t index = 0;
        ;
        index++
    )
    {
        char character = text[index];

        if (
            character >= '0' &&
            character <= '9'
        )
        {
            has_digit = true;

            octets[octet] =
                octets[octet] * 10U +
                (uint32_t)(character - '0');

            if (octets[octet] > 255)
            {
                return false;
            }

            continue;
        }

        if (
            character == '.' &&
            octet < 3 &&
            has_digit
        )
        {
            octet++;
            has_digit = false;
            continue;
        }

        if (
            character == '\0' &&
            octet == 3 &&
            has_digit
        )
        {
            break;
        }

        return false;
    }

    *address =
        IPV4_ADDRESS(
            octets[0],
            octets[1],
            octets[2],
            octets[3]
        );

    return true;
}

void ipv4_format_address(
    uint32_t address,
    char *buffer,
    size_t capacity
)
{
    if (
        buffer == NULL ||
        capacity == 0
    )
    {
        return;
    }

    (void)ksnprintf(
        buffer,
        capacity,
        "%u.%u.%u.%u",
        (uint32_t)((address >> 24) & 0xFF),
        (uint32_t)((address >> 16) & 0xFF),
        (uint32_t)((address >> 8) & 0xFF),
        (uint32_t)(address & 0xFF)
    );
}

bool ipv4_send_from(
    uint32_t source,
    uint32_t destination,
    uint8_t protocol,
    const void *payload,
    size_t length
)
{
    if (
        (payload == NULL && length != 0) ||
        length >
            IPV4_MAX_PACKET_LENGTH -
            IPV4_HEADER_MIN_LENGTH
    )
    {
        return false;
    }

    static const uint8_t broadcast_mac[6] = {
        0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF
    };

    uint8_t destination_mac[6];

    if (destination == 0xFFFFFFFFU)
    {
        memcpy(
            destination_mac,
            broadcast_mac,
            sizeof(destination_mac)
        );
    }
    else
    {
        uint32_t next_hop = destination;

        if (
            local_mask != 0 &&
            (destination & local_mask) !=
                (local_ip & local_mask)
        )
        {
            next_hop = default_gateway;
        }

        if (
            next_hop == 0 ||
            !arp_resolve(
                next_hop,
                destination_mac,
                1000
            )
        )
        {
            return false;
        }
    }

    uint8_t packet[IPV4_MAX_PACKET_LENGTH];

    ipv4_header_t *header =
        (ipv4_header_t *)packet;

    header->version_ihl = 0x45;
    header->dscp_ecn = 0;

    write_be16(
        header->total_length,
        (uint16_t)(
            IPV4_HEADER_MIN_LENGTH +
            length
        )
    );

    write_be16(
        header->identification,
        next_identification++
    );

    write_be16(
        header->flags_fragment,
        0x4000
    );

    header->ttl = 64;
    header->protocol = protocol;

    write_be16(
        header->checksum,
        0
    );

    write_ipv4(
        header->source,
        source
    );

    write_ipv4(
        header->destination,
        destination
    );

    uint16_t header_checksum =
        checksum(
            packet,
            IPV4_HEADER_MIN_LENGTH
        );

    write_be16(
        header->checksum,
        header_checksum
    );

    if (length != 0)
    {
        memcpy(
            packet + IPV4_HEADER_MIN_LENGTH,
            payload,
            length
        );
    }

    return ethernet_send(
        destination_mac,
        ETHERNET_TYPE_IPV4,
        packet,
        IPV4_HEADER_MIN_LENGTH + length
    );
}

bool ipv4_send(
    uint32_t destination,
    uint8_t protocol,
    const void *payload,
    size_t length
)
{
    return ipv4_send_from(
        local_ip,
        destination,
        protocol,
        payload,
        length
    );
}

void ipv4_receive(
    const uint8_t *packet,
    size_t length
)
{
    if (
        packet == NULL ||
        length < IPV4_HEADER_MIN_LENGTH
    )
    {
        return;
    }

    uint8_t version =
        packet[0] >> 4;

    uint8_t ihl_words =
        packet[0] & 0x0F;

    size_t header_length =
        (size_t)ihl_words * 4U;

    if (
        version != 4 ||
        header_length < IPV4_HEADER_MIN_LENGTH ||
        header_length > length
    )
    {
        return;
    }

    uint16_t total_length =
        read_be16(&packet[2]);

    if (
        total_length < header_length ||
        total_length > length
    )
    {
        return;
    }

    if (
        checksum(packet, header_length) != 0
    )
    {
        return;
    }

    uint16_t flags_fragment =
        read_be16(&packet[6]);

    if (
        (flags_fragment & 0x1FFFU) != 0 ||
        (flags_fragment & 0x2000U) != 0
    )
    {
        return;
    }

    uint32_t destination =
        read_ipv4(&packet[16]);

    if (
        destination != local_ip &&
        destination != 0xFFFFFFFFU
    )
    {
        return;
    }

    uint32_t source =
        read_ipv4(&packet[12]);

    const uint8_t *payload =
        packet + header_length;

    size_t payload_length =
        total_length - header_length;

    if (packet[9] == IPV4_PROTOCOL_ICMP)
    {
        icmp_receive(
            source,
            payload,
            payload_length
        );
    }
    else if (packet[9] == IPV4_PROTOCOL_TCP)
    {
        tcp_receive(
            source,
            destination,
            payload,
            payload_length
        );
    }
    else if (packet[9] == IPV4_PROTOCOL_UDP)
    {
        udp_receive(
            source,
            destination,
            payload,
            payload_length
        );
    }
}

void ipv4_print_config(void)
{
    kprintf(
        "IPv4 address: %u.%u.%u.%u\n",
        (uint32_t)((local_ip >> 24) & 0xFF),
        (uint32_t)((local_ip >> 16) & 0xFF),
        (uint32_t)((local_ip >> 8) & 0xFF),
        (uint32_t)(local_ip & 0xFF)
    );

    kprintf(
        "Netmask: %u.%u.%u.%u\n",
        (uint32_t)((local_mask >> 24) & 0xFF),
        (uint32_t)((local_mask >> 16) & 0xFF),
        (uint32_t)((local_mask >> 8) & 0xFF),
        (uint32_t)(local_mask & 0xFF)
    );

    kprintf(
        "Gateway: %u.%u.%u.%u\n",
        (uint32_t)((default_gateway >> 24) & 0xFF),
        (uint32_t)((default_gateway >> 16) & 0xFF),
        (uint32_t)((default_gateway >> 8) & 0xFF),
        (uint32_t)(default_gateway & 0xFF)
    );
}
