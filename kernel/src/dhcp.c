#include "dhcp.h"

#include "ipv4.h"
#include "kstdio.h"
#include "memory.h"
#include "network.h"
#include "terminal.h"
#include "timer.h"
#include "udp.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DHCP_CLIENT_PORT 68
#define DHCP_SERVER_PORT 67

#define DHCP_BOOTREQUEST 1
#define DHCP_BOOTREPLY   2

#define DHCP_HARDWARE_ETHERNET 1
#define DHCP_ETHERNET_LENGTH   6

#define DHCP_FIXED_LENGTH 236
#define DHCP_COOKIE_OFFSET 236
#define DHCP_OPTIONS_OFFSET 240
#define DHCP_PACKET_CAPACITY 548

#define DHCP_MAGIC_COOKIE 0x63825363U

#define DHCP_OPTION_PAD              0
#define DHCP_OPTION_SUBNET_MASK      1
#define DHCP_OPTION_ROUTER           3
#define DHCP_OPTION_DNS_SERVER       6
#define DHCP_OPTION_HOST_NAME        12
#define DHCP_OPTION_REQUESTED_IP     50
#define DHCP_OPTION_LEASE_TIME       51
#define DHCP_OPTION_MESSAGE_TYPE     53
#define DHCP_OPTION_SERVER_ID        54
#define DHCP_OPTION_PARAMETER_LIST   55
#define DHCP_OPTION_CLIENT_ID        61
#define DHCP_OPTION_END              255

#define DHCP_MESSAGE_DISCOVER 1
#define DHCP_MESSAGE_OFFER    2
#define DHCP_MESSAGE_REQUEST  3
#define DHCP_MESSAGE_ACK      5
#define DHCP_MESSAGE_NAK      6

typedef enum
{
    DHCP_STATE_IDLE,
    DHCP_STATE_WAIT_OFFER,
    DHCP_STATE_OFFER_RECEIVED,
    DHCP_STATE_WAIT_ACK,
    DHCP_STATE_ACK_RECEIVED,
    DHCP_STATE_FAILED
} dhcp_state_t;

static volatile dhcp_state_t state;
static uint32_t transaction_id;

static uint32_t offered_address;
static uint32_t offered_server;
static uint32_t offered_netmask;
static uint32_t offered_gateway;
static uint32_t offered_dns;
static uint32_t offered_lease;

static bool configured;
static uint32_t configured_dns;
static uint32_t configured_lease;

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

static void write_be32(
    uint8_t *data,
    uint32_t value
)
{
    data[0] =
        (uint8_t)(value >> 24);

    data[1] =
        (uint8_t)(value >> 16);

    data[2] =
        (uint8_t)(value >> 8);

    data[3] =
        (uint8_t)value;
}

static size_t append_option(
    uint8_t *packet,
    size_t offset,
    uint8_t type,
    const void *value,
    uint8_t length
)
{
    if (
        offset + 2U + length >=
        DHCP_PACKET_CAPACITY
    )
    {
        return 0;
    }

    packet[offset++] = type;
    packet[offset++] = length;

    if (length != 0)
    {
        memcpy(
            &packet[offset],
            value,
            length
        );
    }

    return offset + length;
}

static size_t build_base_packet(
    uint8_t *packet
)
{
    memset(
        packet,
        0,
        DHCP_PACKET_CAPACITY
    );

    packet[0] = DHCP_BOOTREQUEST;
    packet[1] = DHCP_HARDWARE_ETHERNET;
    packet[2] = DHCP_ETHERNET_LENGTH;
    packet[3] = 0;

    write_be32(
        &packet[4],
        transaction_id
    );

    write_be16(
        &packet[10],
        0x8000
    );

    memcpy(
        &packet[28],
        network_mac_address(),
        6
    );

    write_be32(
        &packet[DHCP_COOKIE_OFFSET],
        DHCP_MAGIC_COOKIE
    );

    return DHCP_OPTIONS_OFFSET;
}

static bool send_discover(void)
{
    uint8_t packet[DHCP_PACKET_CAPACITY];
    size_t offset = build_base_packet(packet);

    uint8_t message_type =
        DHCP_MESSAGE_DISCOVER;

    offset = append_option(
        packet,
        offset,
        DHCP_OPTION_MESSAGE_TYPE,
        &message_type,
        1
    );

    if (offset == 0)
    {
        return false;
    }

    uint8_t client_id[7];
    client_id[0] = DHCP_HARDWARE_ETHERNET;

    memcpy(
        &client_id[1],
        network_mac_address(),
        6
    );

    offset = append_option(
        packet,
        offset,
        DHCP_OPTION_CLIENT_ID,
        client_id,
        sizeof(client_id)
    );

    static const uint8_t hostname[] = {
        'L', 'a', 't', 't', 'e', 'r', 'O', 'S'
    };

    offset = append_option(
        packet,
        offset,
        DHCP_OPTION_HOST_NAME,
        hostname,
        sizeof(hostname)
    );

    static const uint8_t requested[] = {
        DHCP_OPTION_SUBNET_MASK,
        DHCP_OPTION_ROUTER,
        DHCP_OPTION_DNS_SERVER,
        DHCP_OPTION_LEASE_TIME
    };

    offset = append_option(
        packet,
        offset,
        DHCP_OPTION_PARAMETER_LIST,
        requested,
        sizeof(requested)
    );

    if (
        offset == 0 ||
        offset >= DHCP_PACKET_CAPACITY
    )
    {
        return false;
    }

    packet[offset++] = DHCP_OPTION_END;

    size_t packet_length =
        offset < 300 ? 300 : offset;

    return udp_send_from(
        0,
        0xFFFFFFFFU,
        DHCP_CLIENT_PORT,
        DHCP_SERVER_PORT,
        packet,
        packet_length
    );
}

static bool send_request(void)
{
    uint8_t packet[DHCP_PACKET_CAPACITY];
    size_t offset = build_base_packet(packet);

    uint8_t message_type =
        DHCP_MESSAGE_REQUEST;

    offset = append_option(
        packet,
        offset,
        DHCP_OPTION_MESSAGE_TYPE,
        &message_type,
        1
    );

    uint8_t requested_ip[4];
    write_be32(
        requested_ip,
        offered_address
    );

    offset = append_option(
        packet,
        offset,
        DHCP_OPTION_REQUESTED_IP,
        requested_ip,
        sizeof(requested_ip)
    );

    uint8_t server_id[4];
    write_be32(
        server_id,
        offered_server
    );

    offset = append_option(
        packet,
        offset,
        DHCP_OPTION_SERVER_ID,
        server_id,
        sizeof(server_id)
    );

    uint8_t client_id[7];
    client_id[0] = DHCP_HARDWARE_ETHERNET;

    memcpy(
        &client_id[1],
        network_mac_address(),
        6
    );

    offset = append_option(
        packet,
        offset,
        DHCP_OPTION_CLIENT_ID,
        client_id,
        sizeof(client_id)
    );

    static const uint8_t requested[] = {
        DHCP_OPTION_SUBNET_MASK,
        DHCP_OPTION_ROUTER,
        DHCP_OPTION_DNS_SERVER,
        DHCP_OPTION_LEASE_TIME
    };

    offset = append_option(
        packet,
        offset,
        DHCP_OPTION_PARAMETER_LIST,
        requested,
        sizeof(requested)
    );

    if (
        offset == 0 ||
        offset >= DHCP_PACKET_CAPACITY
    )
    {
        return false;
    }

    packet[offset++] = DHCP_OPTION_END;

    size_t packet_length =
        offset < 300 ? 300 : offset;

    return udp_send_from(
        0,
        0xFFFFFFFFU,
        DHCP_CLIENT_PORT,
        DHCP_SERVER_PORT,
        packet,
        packet_length
    );
}

static void parse_options(
    const uint8_t *packet,
    size_t length,
    uint8_t *message_type,
    uint32_t *server,
    uint32_t *netmask,
    uint32_t *gateway,
    uint32_t *dns,
    uint32_t *lease
)
{
    size_t offset = DHCP_OPTIONS_OFFSET;

    while (offset < length)
    {
        uint8_t type = packet[offset++];

        if (type == DHCP_OPTION_PAD)
        {
            continue;
        }

        if (type == DHCP_OPTION_END)
        {
            return;
        }

        if (offset >= length)
        {
            return;
        }

        uint8_t option_length =
            packet[offset++];

        if (offset + option_length > length)
        {
            return;
        }

        const uint8_t *value =
            &packet[offset];

        if (
            type == DHCP_OPTION_MESSAGE_TYPE &&
            option_length >= 1
        )
        {
            *message_type = value[0];
        }
        else if (
            type == DHCP_OPTION_SERVER_ID &&
            option_length >= 4
        )
        {
            *server = read_be32(value);
        }
        else if (
            type == DHCP_OPTION_SUBNET_MASK &&
            option_length >= 4
        )
        {
            *netmask = read_be32(value);
        }
        else if (
            type == DHCP_OPTION_ROUTER &&
            option_length >= 4
        )
        {
            *gateway = read_be32(value);
        }
        else if (
            type == DHCP_OPTION_DNS_SERVER &&
            option_length >= 4
        )
        {
            *dns = read_be32(value);
        }
        else if (
            type == DHCP_OPTION_LEASE_TIME &&
            option_length >= 4
        )
        {
            *lease = read_be32(value);
        }

        offset += option_length;
    }
}

static void dhcp_receive(
    uint32_t source_address,
    uint16_t source_port,
    uint16_t destination_port,
    const uint8_t *packet,
    size_t length
)
{
    (void)source_address;
    (void)destination_port;

    if (
        source_port != DHCP_SERVER_PORT ||
        packet == NULL ||
        length < DHCP_OPTIONS_OFFSET ||
        packet[0] != DHCP_BOOTREPLY ||
        packet[1] != DHCP_HARDWARE_ETHERNET ||
        packet[2] != DHCP_ETHERNET_LENGTH ||
        read_be32(&packet[4]) !=
            transaction_id ||
        read_be32(
            &packet[DHCP_COOKIE_OFFSET]
        ) != DHCP_MAGIC_COOKIE
    )
    {
        return;
    }

    if (
        memcmp(
            &packet[28],
            network_mac_address(),
            6
        ) != 0
    )
    {
        return;
    }

    uint8_t message_type = 0;
    uint32_t server = 0;
    uint32_t netmask = 0;
    uint32_t gateway = 0;
    uint32_t dns = 0;
    uint32_t lease = 0;

    parse_options(
        packet,
        length,
        &message_type,
        &server,
        &netmask,
        &gateway,
        &dns,
        &lease
    );

    if (
        state == DHCP_STATE_WAIT_OFFER &&
        message_type == DHCP_MESSAGE_OFFER
    )
    {
        offered_address =
            read_be32(&packet[16]);

        offered_server =
            server != 0 ?
            server :
            read_be32(&packet[20]);

        offered_netmask = netmask;
        offered_gateway = gateway;
        offered_dns = dns;
        offered_lease = lease;

        state = DHCP_STATE_OFFER_RECEIVED;
        return;
    }

    if (
        state == DHCP_STATE_WAIT_ACK &&
        message_type == DHCP_MESSAGE_ACK
    )
    {
        uint32_t acknowledged_address =
            read_be32(&packet[16]);

        if (acknowledged_address != 0)
        {
            offered_address =
                acknowledged_address;
        }

        if (server != 0)
        {
            offered_server = server;
        }

        if (netmask != 0)
        {
            offered_netmask = netmask;
        }

        if (gateway != 0)
        {
            offered_gateway = gateway;
        }

        if (dns != 0)
        {
            offered_dns = dns;
        }

        if (lease != 0)
        {
            offered_lease = lease;
        }

        state = DHCP_STATE_ACK_RECEIVED;
        return;
    }

    if (
        state == DHCP_STATE_WAIT_ACK &&
        message_type == DHCP_MESSAGE_NAK
    )
    {
        state = DHCP_STATE_FAILED;
    }
}

static bool wait_for_state(
    dhcp_state_t wanted,
    uint32_t iterations
)
{
    for (
        uint32_t iteration = 0;
        iteration < iterations;
        iteration++
    )
    {
        network_poll();

        if (state == wanted)
        {
            return true;
        }

        if (state == DHCP_STATE_FAILED)
        {
            return false;
        }

        __asm__ volatile("pause");
    }

    return state == wanted;
}

void dhcp_init(void)
{
    state = DHCP_STATE_IDLE;
    transaction_id = 0;

    offered_address = 0;
    offered_server = 0;
    offered_netmask = 0;
    offered_gateway = 0;
    offered_dns = 0;
    offered_lease = 0;

    configured = false;
    configured_dns =
        IPV4_ADDRESS(10, 0, 2, 3);

    configured_lease = 0;

    (void)udp_bind(
        DHCP_CLIENT_PORT,
        dhcp_receive
    );
}

bool dhcp_configure(
    uint32_t timeout_iterations
)
{
    if (!network_is_ready())
    {
        return false;
    }

    if (timeout_iterations == 0)
    {
        timeout_iterations = 5000000U;
    }

    transaction_id =
        0x4C4F0000U ^
        (uint32_t)timer_ticks() ^
        ((uint32_t)network_mac_address()[4] << 8) ^
        (uint32_t)network_mac_address()[5];

    offered_address = 0;
    offered_server = 0;
    offered_netmask = 0;
    offered_gateway = 0;
    offered_dns = 0;
    offered_lease = 0;

    state = DHCP_STATE_WAIT_OFFER;

    if (!send_discover())
    {
        state = DHCP_STATE_FAILED;
        return false;
    }

    if (
        !wait_for_state(
            DHCP_STATE_OFFER_RECEIVED,
            timeout_iterations
        )
    )
    {
        state = DHCP_STATE_FAILED;
        return false;
    }

    if (
        offered_address == 0 ||
        offered_server == 0
    )
    {
        state = DHCP_STATE_FAILED;
        return false;
    }

    state = DHCP_STATE_WAIT_ACK;

    if (!send_request())
    {
        state = DHCP_STATE_FAILED;
        return false;
    }

    if (
        !wait_for_state(
            DHCP_STATE_ACK_RECEIVED,
            timeout_iterations
        )
    )
    {
        state = DHCP_STATE_FAILED;
        return false;
    }

    if (offered_netmask == 0)
    {
        offered_netmask =
            IPV4_ADDRESS(255, 255, 255, 0);
    }

    if (offered_gateway == 0)
    {
        offered_gateway = offered_server;
    }

    if (offered_dns == 0)
    {
        offered_dns =
            IPV4_ADDRESS(10, 0, 2, 3);
    }

    ipv4_init(
        offered_address,
        offered_netmask,
        offered_gateway
    );

    configured = true;
    configured_dns = offered_dns;
    configured_lease = offered_lease;
    state = DHCP_STATE_IDLE;

    return true;
}

bool dhcp_is_configured(void)
{
    return configured;
}

uint32_t dhcp_dns_server(void)
{
    return configured_dns;
}

uint32_t dhcp_lease_seconds(void)
{
    return configured_lease;
}

void dhcp_print_status(void)
{
    if (!configured)
    {
        terminal_write_line(
            "DHCP: no active lease"
        );
        return;
    }

    char address[16];
    char dns[16];

    ipv4_format_address(
        ipv4_local_address(),
        address,
        sizeof(address)
    );

    ipv4_format_address(
        configured_dns,
        dns,
        sizeof(dns)
    );

    kprintf(
        "DHCP lease: address=%s dns=%s seconds=%u\n",
        address,
        dns,
        configured_lease
    );
}
