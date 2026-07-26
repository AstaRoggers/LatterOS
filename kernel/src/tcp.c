#include "tcp.h"

#include "ipv4.h"
#include "kstdio.h"
#include "memory.h"
#include "network.h"
#include "timer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TCP_HEADER_LENGTH       20U
#define TCP_MAX_PAYLOAD         1200U
#define TCP_RECEIVE_BUFFER_SIZE 8192U
#define TCP_WINDOW_SIZE         8192U

#define TCP_FLAG_FIN 0x01U
#define TCP_FLAG_SYN 0x02U
#define TCP_FLAG_RST 0x04U
#define TCP_FLAG_PSH 0x08U
#define TCP_FLAG_ACK 0x10U

#define TCP_DEFAULT_TIMEOUT_MS 3000U
#define TCP_RETRY_INTERVAL_MS  500U

#define TCP_SEQUENCE_LESS(a, b) \
    ((int32_t)((a) - (b)) < 0)

#define TCP_SEQUENCE_LESS_EQUAL(a, b) \
    ((int32_t)((a) - (b)) <= 0)

typedef struct
{
    tcp_state_t state;

    uint32_t local_address;
    uint32_t remote_address;

    uint16_t local_port;
    uint16_t remote_port;

    uint32_t send_unacknowledged;
    uint32_t send_next;
    uint32_t receive_next;

    bool reset_received;
    bool peer_has_closed;

    uint8_t receive_buffer[TCP_RECEIVE_BUFFER_SIZE];
    size_t receive_length;
} tcp_connection_t;

static tcp_connection_t connection;
static uint16_t next_ephemeral_port;
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
    data[0] = (uint8_t)(value >> 8);
    data[1] = (uint8_t)value;
}

static void write_be32(
    uint8_t *data,
    uint32_t value
)
{
    data[0] = (uint8_t)(value >> 24);
    data[1] = (uint8_t)(value >> 16);
    data[2] = (uint8_t)(value >> 8);
    data[3] = (uint8_t)value;
}

static uint32_t checksum_add(
    uint32_t sum,
    const uint8_t *data,
    size_t length
)
{
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

    return sum;
}

static uint16_t checksum_finish(uint32_t sum)
{
    while (sum >> 16)
    {
        sum =
            (sum & 0xFFFFU) +
            (sum >> 16);
    }

    return (uint16_t)~sum;
}

static uint16_t tcp_checksum(
    uint32_t source_address,
    uint32_t destination_address,
    const uint8_t *segment,
    size_t length
)
{
    uint8_t pseudo_header[12];

    write_be32(
        &pseudo_header[0],
        source_address
    );

    write_be32(
        &pseudo_header[4],
        destination_address
    );

    pseudo_header[8] = 0;
    pseudo_header[9] = IPV4_PROTOCOL_TCP;

    write_be16(
        &pseudo_header[10],
        (uint16_t)length
    );

    uint32_t sum = 0;

    sum = checksum_add(
        sum,
        pseudo_header,
        sizeof(pseudo_header)
    );

    sum = checksum_add(
        sum,
        segment,
        length
    );

    return checksum_finish(sum);
}

static uint64_t milliseconds_to_ticks(
    uint32_t milliseconds
)
{
    uint32_t frequency = timer_frequency();

    if (frequency == 0)
    {
        return milliseconds;
    }

    uint64_t ticks =
        ((uint64_t)milliseconds * frequency + 999U) /
        1000U;

    return ticks == 0 ? 1 : ticks;
}

static bool timeout_expired(
    uint64_t start,
    uint32_t timeout_ms
)
{
    return
        timer_ticks() - start >=
        milliseconds_to_ticks(timeout_ms);
}

static bool send_segment(
    uint32_t sequence,
    uint32_t acknowledgement,
    uint8_t flags,
    const void *payload,
    size_t payload_length
)
{
    if (
        connection.remote_address == 0 ||
        connection.local_port == 0 ||
        connection.remote_port == 0 ||
        (payload == NULL && payload_length != 0) ||
        payload_length > TCP_MAX_PAYLOAD
    )
    {
        return false;
    }

    uint8_t segment[
        TCP_HEADER_LENGTH +
        TCP_MAX_PAYLOAD
    ];

    memset(segment, 0, sizeof(segment));

    write_be16(
        &segment[0],
        connection.local_port
    );

    write_be16(
        &segment[2],
        connection.remote_port
    );

    write_be32(
        &segment[4],
        sequence
    );

    write_be32(
        &segment[8],
        acknowledgement
    );

    segment[12] =
        (uint8_t)(5U << 4);

    segment[13] = flags;

    write_be16(
        &segment[14],
        TCP_WINDOW_SIZE
    );

    write_be16(&segment[16], 0);
    write_be16(&segment[18], 0);

    if (payload_length != 0)
    {
        memcpy(
            &segment[TCP_HEADER_LENGTH],
            payload,
            payload_length
        );
    }

    size_t segment_length =
        TCP_HEADER_LENGTH +
        payload_length;

    uint16_t checksum =
        tcp_checksum(
            connection.local_address,
            connection.remote_address,
            segment,
            segment_length
        );

    write_be16(
        &segment[16],
        checksum
    );

    if (
        !ipv4_send(
            connection.remote_address,
            IPV4_PROTOCOL_TCP,
            segment,
            segment_length
        )
    )
    {
        return false;
    }

    transmitted_count++;
    return true;
}

static bool send_acknowledgement(void)
{
    return send_segment(
        connection.send_next,
        connection.receive_next,
        TCP_FLAG_ACK,
        NULL,
        0
    );
}

static void reset_connection(void)
{
    connection.state = TCP_STATE_CLOSED;
    connection.local_address = 0;
    connection.remote_address = 0;
    connection.local_port = 0;
    connection.remote_port = 0;
    connection.send_unacknowledged = 0;
    connection.send_next = 0;
    connection.receive_next = 0;
    connection.reset_received = false;
    connection.peer_has_closed = false;
    connection.receive_length = 0;
}

void tcp_init(void)
{
    reset_connection();

    next_ephemeral_port =
        (uint16_t)(49152U +
        (timer_ticks() & 0x0FFFU));

    received_count = 0;
    transmitted_count = 0;
}

bool tcp_connect(
    uint32_t destination_address,
    uint16_t destination_port,
    uint32_t timeout_ms
)
{
    if (
        !network_is_ready() ||
        destination_address == 0 ||
        destination_port == 0 ||
        connection.state != TCP_STATE_CLOSED
    )
    {
        return false;
    }

    reset_connection();

    connection.local_address =
        ipv4_local_address();

    connection.remote_address =
        destination_address;

    connection.remote_port =
        destination_port;

    connection.local_port =
        next_ephemeral_port++;

    if (next_ephemeral_port < 49152U)
    {
        next_ephemeral_port = 49152U;
    }

    uint32_t initial_sequence =
        (uint32_t)(
            timer_ticks() * 1103515245ULL +
            destination_address +
            destination_port
        );

    connection.send_unacknowledged =
        initial_sequence;

    connection.send_next =
        initial_sequence + 1U;

    connection.state =
        TCP_STATE_SYN_SENT;

    if (timeout_ms == 0)
    {
        timeout_ms = TCP_DEFAULT_TIMEOUT_MS;
    }

    uint64_t start = timer_ticks();
    uint64_t last_send = 0;
    bool first_send = true;

    while (
        connection.state ==
            TCP_STATE_SYN_SENT &&
        !connection.reset_received
    )
    {
        uint64_t now = timer_ticks();

        if (
            first_send ||
            now - last_send >=
                milliseconds_to_ticks(
                    TCP_RETRY_INTERVAL_MS
                )
        )
        {
            if (
                !send_segment(
                    initial_sequence,
                    0,
                    TCP_FLAG_SYN,
                    NULL,
                    0
                )
            )
            {
                reset_connection();
                return false;
            }

            first_send = false;
            last_send = now;
        }

        network_poll();

        if (timeout_expired(start, timeout_ms))
        {
            break;
        }

        __asm__ volatile("pause");
    }

    if (
        connection.state !=
        TCP_STATE_ESTABLISHED
    )
    {
        reset_connection();
        return false;
    }

    return true;
}

bool tcp_send_data(
    const void *data,
    size_t length,
    uint32_t timeout_ms
)
{
    if (
        connection.state !=
            TCP_STATE_ESTABLISHED ||
        data == NULL ||
        length == 0 ||
        length > TCP_MAX_PAYLOAD
    )
    {
        return false;
    }

    if (timeout_ms == 0)
    {
        timeout_ms = TCP_DEFAULT_TIMEOUT_MS;
    }

    uint32_t sequence =
        connection.send_next;

    uint32_t target_acknowledgement =
        sequence + (uint32_t)length;

    connection.send_next =
        target_acknowledgement;

    uint64_t start = timer_ticks();
    uint64_t last_send = 0;
    bool first_send = true;

    while (
        TCP_SEQUENCE_LESS(
            connection.send_unacknowledged,
            target_acknowledgement
        ) &&
        connection.state != TCP_STATE_CLOSED &&
        !connection.reset_received
    )
    {
        uint64_t now = timer_ticks();

        if (
            first_send ||
            now - last_send >=
                milliseconds_to_ticks(
                    TCP_RETRY_INTERVAL_MS
                )
        )
        {
            if (
                !send_segment(
                    sequence,
                    connection.receive_next,
                    TCP_FLAG_ACK |
                    TCP_FLAG_PSH,
                    data,
                    length
                )
            )
            {
                connection.send_next = sequence;
                return false;
            }

            first_send = false;
            last_send = now;
        }

        network_poll();

        if (timeout_expired(start, timeout_ms))
        {
            return false;
        }

        __asm__ volatile("pause");
    }

    return
        !connection.reset_received &&
        TCP_SEQUENCE_LESS_EQUAL(
            target_acknowledgement,
            connection.send_unacknowledged
        );
}

size_t tcp_receive_data(
    void *buffer,
    size_t capacity,
    uint32_t timeout_ms
)
{
    if (
        buffer == NULL ||
        capacity == 0 ||
        connection.state == TCP_STATE_CLOSED
    )
    {
        return 0;
    }

    if (timeout_ms == 0)
    {
        timeout_ms = TCP_DEFAULT_TIMEOUT_MS;
    }

    uint64_t start = timer_ticks();

    while (
        connection.receive_length == 0 &&
        !connection.peer_has_closed &&
        !connection.reset_received &&
        connection.state != TCP_STATE_CLOSED
    )
    {
        network_poll();

        if (timeout_expired(start, timeout_ms))
        {
            return 0;
        }

        __asm__ volatile("pause");
    }

    size_t copy_length =
        connection.receive_length;

    if (copy_length > capacity)
    {
        copy_length = capacity;
    }

    if (copy_length == 0)
    {
        return 0;
    }

    memcpy(
        buffer,
        connection.receive_buffer,
        copy_length
    );

    size_t remaining =
        connection.receive_length -
        copy_length;

    if (remaining != 0)
    {
        memmove(
            connection.receive_buffer,
            &connection.receive_buffer[copy_length],
            remaining
        );
    }

    connection.receive_length = remaining;
    return copy_length;
}

void tcp_close(uint32_t timeout_ms)
{
    if (connection.state == TCP_STATE_CLOSED)
    {
        return;
    }

    if (timeout_ms == 0)
    {
        timeout_ms = 1000U;
    }

    if (
        connection.state ==
        TCP_STATE_ESTABLISHED
    )
    {
        uint32_t sequence =
            connection.send_next;

        connection.send_next++;
        connection.state =
            TCP_STATE_FIN_WAIT_1;

        (void)send_segment(
            sequence,
            connection.receive_next,
            TCP_FLAG_FIN |
            TCP_FLAG_ACK,
            NULL,
            0
        );
    }
    else if (
        connection.state ==
        TCP_STATE_CLOSE_WAIT
    )
    {
        uint32_t sequence =
            connection.send_next;

        connection.send_next++;
        connection.state =
            TCP_STATE_LAST_ACK;

        (void)send_segment(
            sequence,
            connection.receive_next,
            TCP_FLAG_FIN |
            TCP_FLAG_ACK,
            NULL,
            0
        );
    }

    uint64_t start = timer_ticks();

    while (
        connection.state != TCP_STATE_CLOSED &&
        !connection.reset_received &&
        !timeout_expired(start, timeout_ms)
    )
    {
        network_poll();
        __asm__ volatile("pause");
    }

    reset_connection();
}

void tcp_abort(void)
{
    if (
        connection.state != TCP_STATE_CLOSED &&
        connection.remote_address != 0
    )
    {
        (void)send_segment(
            connection.send_next,
            connection.receive_next,
            TCP_FLAG_RST |
            TCP_FLAG_ACK,
            NULL,
            0
        );
    }

    reset_connection();
}

bool tcp_is_connected(void)
{
    return
        connection.state ==
            TCP_STATE_ESTABLISHED ||
        connection.state ==
            TCP_STATE_CLOSE_WAIT;
}

bool tcp_peer_closed(void)
{
    return connection.peer_has_closed;
}

tcp_state_t tcp_state(void)
{
    return connection.state;
}

void tcp_receive(
    uint32_t source_address,
    uint32_t destination_address,
    const uint8_t *packet,
    size_t length
)
{
    if (
        packet == NULL ||
        length < TCP_HEADER_LENGTH ||
        destination_address !=
            connection.local_address ||
        source_address !=
            connection.remote_address
    )
    {
        return;
    }

    uint16_t source_port =
        read_be16(&packet[0]);

    uint16_t destination_port =
        read_be16(&packet[2]);

    if (
        source_port != connection.remote_port ||
        destination_port != connection.local_port
    )
    {
        return;
    }

    uint8_t header_words =
        packet[12] >> 4;

    size_t header_length =
        (size_t)header_words * 4U;

    if (
        header_length < TCP_HEADER_LENGTH ||
        header_length > length
    )
    {
        return;
    }

    if (
        tcp_checksum(
            source_address,
            destination_address,
            packet,
            length
        ) != 0
    )
    {
        return;
    }

    received_count++;

    uint32_t sequence =
        read_be32(&packet[4]);

    uint32_t acknowledgement =
        read_be32(&packet[8]);

    uint8_t flags = packet[13];

    if (flags & TCP_FLAG_RST)
    {
        connection.reset_received = true;
        connection.state = TCP_STATE_CLOSED;
        return;
    }

    if (
        connection.state ==
            TCP_STATE_SYN_SENT
    )
    {
        if (
            (flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) ==
                (TCP_FLAG_SYN | TCP_FLAG_ACK) &&
            acknowledgement ==
                connection.send_next
        )
        {
            connection.send_unacknowledged =
                acknowledgement;

            connection.receive_next =
                sequence + 1U;

            connection.state =
                TCP_STATE_ESTABLISHED;

            (void)send_acknowledgement();
        }

        return;
    }

    if (flags & TCP_FLAG_ACK)
    {
        if (
            TCP_SEQUENCE_LESS(
                connection.send_unacknowledged,
                acknowledgement
            ) &&
            TCP_SEQUENCE_LESS_EQUAL(
                acknowledgement,
                connection.send_next
            )
        )
        {
            connection.send_unacknowledged =
                acknowledgement;
        }

        if (
            connection.state ==
                TCP_STATE_FIN_WAIT_1 &&
            acknowledgement ==
                connection.send_next
        )
        {
            connection.state =
                TCP_STATE_FIN_WAIT_2;
        }
        else if (
            connection.state ==
                TCP_STATE_LAST_ACK &&
            acknowledgement ==
                connection.send_next
        )
        {
            connection.state =
                TCP_STATE_CLOSED;
        }
    }

    const uint8_t *payload =
        &packet[header_length];

    size_t payload_length =
        length - header_length;

    bool acknowledgement_needed = false;

    if (payload_length != 0)
    {
        if (sequence == connection.receive_next)
        {
            size_t available =
                TCP_RECEIVE_BUFFER_SIZE -
                connection.receive_length;

            size_t copy_length =
                payload_length;

            if (copy_length > available)
            {
                copy_length = available;
            }

            if (copy_length != 0)
            {
                memcpy(
                    &connection.receive_buffer[
                        connection.receive_length
                    ],
                    payload,
                    copy_length
                );

                connection.receive_length +=
                    copy_length;
            }

            connection.receive_next +=
                (uint32_t)payload_length;
        }

        acknowledgement_needed = true;
    }

    if (flags & TCP_FLAG_FIN)
    {
        uint32_t fin_sequence =
            sequence +
            (uint32_t)payload_length;

        if (fin_sequence == connection.receive_next)
        {
            connection.receive_next++;
            connection.peer_has_closed = true;

            if (
                connection.state ==
                    TCP_STATE_ESTABLISHED
            )
            {
                connection.state =
                    TCP_STATE_CLOSE_WAIT;
            }
            else if (
                connection.state ==
                    TCP_STATE_FIN_WAIT_1 ||
                connection.state ==
                    TCP_STATE_FIN_WAIT_2
            )
            {
                connection.state =
                    TCP_STATE_CLOSED;
            }
        }

        acknowledgement_needed = true;
    }

    if (acknowledgement_needed)
    {
        (void)send_acknowledgement();
    }
}

uint64_t tcp_received_segments(void)
{
    return received_count;
}

uint64_t tcp_transmitted_segments(void)
{
    return transmitted_count;
}

static const char *state_name(tcp_state_t state)
{
    switch (state)
    {
        case TCP_STATE_SYN_SENT:
            return "SYN-SENT";

        case TCP_STATE_ESTABLISHED:
            return "ESTABLISHED";

        case TCP_STATE_FIN_WAIT_1:
            return "FIN-WAIT-1";

        case TCP_STATE_FIN_WAIT_2:
            return "FIN-WAIT-2";

        case TCP_STATE_CLOSE_WAIT:
            return "CLOSE-WAIT";

        case TCP_STATE_LAST_ACK:
            return "LAST-ACK";

        default:
            return "CLOSED";
    }
}

void tcp_print_status(void)
{
    kprintf(
        "TCP: state=%s rx=%llu tx=%llu\n",
        state_name(connection.state),
        (unsigned long long)received_count,
        (unsigned long long)transmitted_count
    );

    if (connection.state != TCP_STATE_CLOSED)
    {
        char remote[16];

        ipv4_format_address(
            connection.remote_address,
            remote,
            sizeof(remote)
        );

        kprintf(
            "Connection: %u -> %s:%u buffered=%zu\n",
            (uint32_t)connection.local_port,
            remote,
            (uint32_t)connection.remote_port,
            connection.receive_length
        );
    }
}
