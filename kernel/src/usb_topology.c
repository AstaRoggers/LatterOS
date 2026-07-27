#include "usb_topology.h"

#include "usb.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define USB_REQUEST_GET_STATUS        0x00U
#define USB_REQUEST_TYPE_HUB_PORT_IN  0xA3U
#define USB_HUB_PORT_CONNECTION       0x0001U
#define USB_HUB_PORT_LOW_SPEED        0x0200U

#define FNV1A_OFFSET 1469598103934665603ULL
#define FNV1A_PRIME  1099511628211ULL

typedef struct __attribute__((packed))
{
    uint16_t status;
    uint16_t change;
} usb_hub_port_status_t;

static uint64_t mix_byte(
    uint64_t hash,
    uint8_t value
)
{
    hash ^= value;
    hash *= FNV1A_PRIME;
    return hash;
}

static uint64_t mix_u16(
    uint64_t hash,
    uint16_t value
)
{
    hash = mix_byte(hash, (uint8_t)value);
    hash = mix_byte(hash, (uint8_t)(value >> 8));
    return hash;
}

static uint64_t mix_u32(
    uint64_t hash,
    uint32_t value
)
{
    for (uint32_t shift = 0; shift < 32; shift += 8)
    {
        hash = mix_byte(
            hash,
            (uint8_t)(value >> shift)
        );
    }

    return hash;
}

bool usb_topology_snapshot(
    usb_topology_snapshot_t *snapshot
)
{
    if (snapshot == NULL || !usb_is_ready())
    {
        return false;
    }

    snapshot->signature = FNV1A_OFFSET;
    snapshot->connected_points =
        usb_connected_port_count();
    snapshot->hub_ports_scanned = 0;
    snapshot->query_failures = 0;

    snapshot->signature = mix_u32(
        snapshot->signature,
        snapshot->connected_points
    );

    uint8_t device_count = usb_device_count();

    snapshot->signature = mix_byte(
        snapshot->signature,
        device_count
    );

    for (
        uint8_t index = 0;
        index < device_count;
        index++
    )
    {
        const usb_device_t *device =
            usb_device(index);

        if (device == NULL || !device->present)
        {
            snapshot->signature = mix_byte(
                snapshot->signature,
                0xFFU
            );
            continue;
        }

        snapshot->signature = mix_byte(
            snapshot->signature,
            device->address
        );
        snapshot->signature = mix_byte(
            snapshot->signature,
            device->parent_address
        );
        snapshot->signature = mix_byte(
            snapshot->signature,
            device->port
        );
        snapshot->signature = mix_u16(
            snapshot->signature,
            device->vendor_id
        );
        snapshot->signature = mix_u16(
            snapshot->signature,
            device->product_id
        );
        snapshot->signature = mix_byte(
            snapshot->signature,
            device->is_hub ? 1U : 0U
        );

        if (!device->is_hub)
        {
            continue;
        }

        snapshot->signature = mix_byte(
            snapshot->signature,
            device->hub_port_count
        );

        for (
            uint8_t port = 1;
            port <= device->hub_port_count;
            port++
        )
        {
            usb_hub_port_status_t status = {0};

            snapshot->hub_ports_scanned++;
            snapshot->signature = mix_byte(
                snapshot->signature,
                port
            );

            bool success =
                usb_control_request_device(
                    device,
                    USB_REQUEST_TYPE_HUB_PORT_IN,
                    USB_REQUEST_GET_STATUS,
                    0,
                    port,
                    &status,
                    sizeof(status)
                );

            if (!success)
            {
                snapshot->query_failures++;
                snapshot->signature = mix_u32(
                    snapshot->signature,
                    0xFFFFFFFFU
                );
                continue;
            }

            uint16_t stable_status =
                status.status &
                (USB_HUB_PORT_CONNECTION |
                 USB_HUB_PORT_LOW_SPEED);

            snapshot->signature = mix_u16(
                snapshot->signature,
                stable_status
            );

            if (
                stable_status &
                USB_HUB_PORT_CONNECTION
            )
            {
                snapshot->connected_points++;
            }
        }
    }

    snapshot->signature = mix_u32(
        snapshot->signature,
        snapshot->connected_points
    );
    snapshot->signature = mix_u32(
        snapshot->signature,
        snapshot->hub_ports_scanned
    );
    snapshot->signature = mix_u32(
        snapshot->signature,
        snapshot->query_failures
    );

    return snapshot->query_failures == 0;
}
