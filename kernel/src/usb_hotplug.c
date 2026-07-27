#include "usb_hotplug.h"

#include "block_device.h"
#include "fat_fs.h"
#include "irq.h"
#include "klog.h"
#include "process.h"
#include "timer.h"
#include "usb.h"
#include "usb_mass_storage.h"
#include "usb_topology.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define USB_HOTPLUG_POLL_TICKS 250ULL

static bool initialized;
static bool worker_created;
static usb_topology_snapshot_t observed_topology;
static uint8_t observed_root_ports;
static uint64_t event_count;
static uint64_t removal_count;
static uint64_t insertion_count;
static uint64_t hub_event_count;

static bool strings_equal(
    const char *first,
    const char *second
)
{
    if (first == NULL || second == NULL)
    {
        return false;
    }

    uint32_t index = 0;

    while (
        first[index] != '\0' &&
        second[index] != '\0'
    )
    {
        if (first[index] != second[index])
        {
            return false;
        }

        index++;
    }

    return first[index] == second[index];
}

static void yield_current_thread(void)
{
    __asm__ volatile(
        "int $0x81"
        :
        :
        : "memory"
    );
}

static bool usb_fat_is_mounted(void)
{
    const char *path = fat_fs_mount_path();

    return strings_equal(path, "/media/usb");
}

static void refresh_observed_topology(void)
{
    usb_topology_snapshot_t refreshed = {0};

    observed_root_ports =
        usb_connected_port_count();

    if (usb_topology_snapshot(&refreshed))
    {
        observed_topology = refreshed;
        return;
    }

    observed_topology.signature = 0;
    observed_topology.connected_points = 0;
    observed_topology.hub_ports_scanned = 0;
    observed_topology.query_failures = 1;
}

static void handle_topology_change(
    const usb_topology_snapshot_t *current
)
{
    uint32_t previous_connections =
        observed_topology.connected_points;

    uint32_t current_connections =
        current != NULL ?
            current->connected_points : 0;

    bool removed =
        current_connections < previous_connections;

    bool inserted =
        current_connections > previous_connections;

    if (usb_fat_is_mounted())
    {
        (void)fat_fs_unmount();
    }

    (void)block_device_unregister_prefix(
        "USB mass storage"
    );

    bool controller_ready = usb_init();

    bool storage_ready =
        controller_ready &&
        usb_mass_storage_init();

    if (
        storage_ready &&
        !fat_fs_mounted()
    )
    {
        (void)fat_fs_mount_first_usb();
    }

    event_count++;

    if (removed)
    {
        removal_count++;
    }

    if (inserted)
    {
        insertion_count++;
    }

    if (
        current != NULL &&
        current->hub_ports_scanned > 0
    )
    {
        hub_event_count++;
    }

    refresh_observed_topology();

    klogf(
        KLOG_INFO,
        "usb-hotplug",
        "event=%llu links=%u devices=%u storage=%u mounted=%s hub-scans=%u failures=%u",
        (unsigned long long)event_count,
        (unsigned int)
            observed_topology.connected_points,
        (unsigned int)usb_device_count(),
        (unsigned int)usb_mass_storage_count(),
        usb_fat_is_mounted() ? "yes" : "no",
        (unsigned int)
            observed_topology.hub_ports_scanned,
        (unsigned int)
            observed_topology.query_failures
    );
}

static void hotplug_worker(void *argument)
{
    (void)argument;

    uint64_t last_poll = timer_ticks();

    for (;;)
    {
        uint64_t now = timer_ticks();

        if (now - last_poll >= USB_HOTPLUG_POLL_TICKS)
        {
            last_poll = now;

            usb_topology_snapshot_t current = {0};

            irq_disable();

            uint8_t current_root_ports =
                usb_connected_port_count();

            bool scanned =
                usb_topology_snapshot(&current);

            bool root_changed =
                current_root_ports !=
                    observed_root_ports;

            bool topology_changed =
                scanned &&
                current.signature !=
                    observed_topology.signature;

            if (root_changed || topology_changed)
            {
                if (!scanned)
                {
                    current.signature = 0;
                    current.connected_points =
                        current_root_ports;
                    current.hub_ports_scanned = 0;
                    current.query_failures = 1;
                }

                handle_topology_change(&current);
            }

            irq_enable();
        }

        yield_current_thread();
    }
}

bool usb_hotplug_init(void)
{
    if (initialized)
    {
        return worker_created;
    }

    initialized = true;
    event_count = 0;
    removal_count = 0;
    insertion_count = 0;
    hub_event_count = 0;

    refresh_observed_topology();

    worker_created =
        process_create_kernel_thread(
            "usb-hotplug",
            hotplug_worker,
            NULL
        );

    return worker_created;
}

uint64_t usb_hotplug_event_count(void)
{
    return event_count;
}

uint64_t usb_hotplug_removal_count(void)
{
    return removal_count;
}

uint64_t usb_hotplug_insertion_count(void)
{
    return insertion_count;
}

uint64_t usb_hotplug_hub_event_count(void)
{
    return hub_event_count;
}
