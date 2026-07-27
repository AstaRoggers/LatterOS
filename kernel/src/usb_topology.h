#ifndef USB_TOPOLOGY_H
#define USB_TOPOLOGY_H

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    uint64_t signature;
    uint32_t connected_points;
    uint32_t hub_ports_scanned;
    uint32_t query_failures;
} usb_topology_snapshot_t;

bool usb_topology_snapshot(
    usb_topology_snapshot_t *snapshot
);

#endif
