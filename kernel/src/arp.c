#include "arp.h"

#include "ethernet.h"
#include "kstdio.h"
#include "memory.h"
#include "network.h"
#include "timer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ARP_HARDWARE_ETHERNET 1
#define ARP_PROTOCOL_IPV4     0x0800

#define ARP_OPERATION_REQUEST 1
#define ARP_OPERATION_REPLY   2

#define ARP_CACHE_SIZE 8

typedef struct __attribute__((packed))
{
    uint8_t hardware_type[2];
    uint8_t protocol_type[2];
    uint8_t hardware_length;
    uint8_t protocol_length;
    uint8_t operation[2];

    uint8_t sender_hardware[6];
    uint8_t sender_protocol[4];

    uint8_t target_hardware[6];
    uint8_t target_protocol[4];
} arp_packet_t;

typedef struct
{
    bool valid;
    uint32_t address;
    uint8_t mac[6];
    uint64_t updated_at;
} arp_entry_t;

static const uint8_t broadcast_mac[6] = {
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF
};

static const uint8_t zero_mac[6] = {
    0, 0, 0, 0, 0, 0
};

static arp_entry_t cache[ARP_CACHE_SIZE];
static uint32_t local_ip;

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

static bool mac_equal(
    const uint8_t first[6],
    const uint8_t second[6]
)
{
    for (uint8_t index = 0; index < 6; index++)
    {
        if (first[index] != second[index])
        {
            return false;
        }
    }

    return true;
}

static void cache_store(
    uint32_t address,
    const uint8_t mac[6]
)
{
    arp_entry_t *entry = NULL;

    for (
        uint8_t index = 0;
        index < ARP_CACHE_SIZE;
        index++
    )
    {
        if (
            cache[index].valid &&
            cache[index].address == address
        )
        {
            entry = &cache[index];
            break;
        }

        if (
            entry == NULL &&
            !cache[index].valid
        )
        {
            entry = &cache[index];
        }
    }

    if (entry == NULL)
    {
        uint8_t oldest = 0;

        for (
            uint8_t index = 1;
            index < ARP_CACHE_SIZE;
            index++
        )
        {
            if (
                cache[index].updated_at <
                cache[oldest].updated_at
            )
            {
                oldest = index;
            }
        }

        entry = &cache[oldest];
    }

    entry->valid = true;
    entry->address = address;
    entry->updated_at = timer_ticks();

    memcpy(
        entry->mac,
        mac,
        6
    );
}

static bool cache_find(
    uint32_t address,
    uint8_t mac[6]
)
{
    for (
        uint8_t index = 0;
        index < ARP_CACHE_SIZE;
        index++
    )
    {
        if (
            cache[index].valid &&
            cache[index].address == address
        )
        {
            memcpy(
                mac,
                cache[index].mac,
                6
            );

            return true;
        }
    }

    return false;
}

static bool send_packet(
    uint16_t operation,
    const uint8_t destination_mac[6],
    uint32_t destination_ip,
    const uint8_t target_mac[6]
)
{
    arp_packet_t packet;

    write_be16(
        packet.hardware_type,
        ARP_HARDWARE_ETHERNET
    );

    write_be16(
        packet.protocol_type,
        ARP_PROTOCOL_IPV4
    );

    packet.hardware_length = 6;
    packet.protocol_length = 4;

    write_be16(
        packet.operation,
        operation
    );

    memcpy(
        packet.sender_hardware,
        network_mac_address(),
        6
    );

    write_ipv4(
        packet.sender_protocol,
        local_ip
    );

    memcpy(
        packet.target_hardware,
        target_mac,
        6
    );

    write_ipv4(
        packet.target_protocol,
        destination_ip
    );

    return ethernet_send(
        destination_mac,
        ETHERNET_TYPE_ARP,
        &packet,
        sizeof(packet)
    );
}

void arp_init(uint32_t local_address)
{
    local_ip = local_address;

    for (
        uint8_t index = 0;
        index < ARP_CACHE_SIZE;
        index++
    )
    {
        cache[index].valid = false;
        cache[index].address = 0;
        cache[index].updated_at = 0;

        memset(
            cache[index].mac,
            0,
            6
        );
    }
}

void arp_receive(
    const uint8_t *packet_data,
    size_t length
)
{
    if (
        packet_data == NULL ||
        length < sizeof(arp_packet_t)
    )
    {
        return;
    }

    const arp_packet_t *packet =
        (const arp_packet_t *)packet_data;

    if (
        read_be16(packet->hardware_type) !=
            ARP_HARDWARE_ETHERNET ||
        read_be16(packet->protocol_type) !=
            ARP_PROTOCOL_IPV4 ||
        packet->hardware_length != 6 ||
        packet->protocol_length != 4
    )
    {
        return;
    }

    uint16_t operation =
        read_be16(packet->operation);

    uint32_t sender_ip =
        read_ipv4(packet->sender_protocol);

    uint32_t target_ip =
        read_ipv4(packet->target_protocol);

    if (
        sender_ip != 0 &&
        !mac_equal(
            packet->sender_hardware,
            zero_mac
        )
    )
    {
        cache_store(
            sender_ip,
            packet->sender_hardware
        );
    }

    if (
        operation == ARP_OPERATION_REQUEST &&
        target_ip == local_ip
    )
    {
        (void)send_packet(
            ARP_OPERATION_REPLY,
            packet->sender_hardware,
            sender_ip,
            packet->sender_hardware
        );
    }
}

bool arp_resolve(
    uint32_t address,
    uint8_t mac[6],
    uint32_t timeout_milliseconds
)
{
    if (cache_find(address, mac))
    {
        return true;
    }

    if (
        !send_packet(
            ARP_OPERATION_REQUEST,
            broadcast_mac,
            address,
            zero_mac
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

    uint64_t start = timer_ticks();

    while (
        timer_ticks() - start <
        timeout_ticks
    )
    {
        network_poll();

        if (cache_find(address, mac))
        {
            return true;
        }

        __asm__ volatile("pause");
    }

    network_poll();

    return cache_find(address, mac);
}

static void print_ip(uint32_t address)
{
    kprintf(
        "%u.%u.%u.%u",
        (uint32_t)((address >> 24) & 0xFF),
        (uint32_t)((address >> 16) & 0xFF),
        (uint32_t)((address >> 8) & 0xFF),
        (uint32_t)(address & 0xFF)
    );
}

void arp_print_cache(void)
{
    bool any = false;

    for (
        uint8_t index = 0;
        index < ARP_CACHE_SIZE;
        index++
    )
    {
        if (!cache[index].valid)
        {
            continue;
        }

        any = true;

        print_ip(cache[index].address);

        kprintf(
            " -> %02X:%02X:%02X:%02X:%02X:%02X\n",
            (uint32_t)cache[index].mac[0],
            (uint32_t)cache[index].mac[1],
            (uint32_t)cache[index].mac[2],
            (uint32_t)cache[index].mac[3],
            (uint32_t)cache[index].mac[4],
            (uint32_t)cache[index].mac[5]
        );
    }

    if (!any)
    {
        kprintf("ARP cache is empty\n");
    }
}
