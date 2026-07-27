#include "storage.h"

#include "ahci.h"
#include "ata.h"
#include "block_device.h"
#include "terminal.h"

#include <stddef.h>
#include <stdint.h>

#define PCI_CLASS_STORAGE       0x01
#define PCI_SUBCLASS_IDE        0x01
#define PCI_SUBCLASS_SATA       0x06
#define PCI_SUBCLASS_NVM        0x08

#define PCI_PROGIF_AHCI         0x01
#define PCI_PROGIF_NVME         0x02

static const pci_device_t *selected_controller;
static storage_controller_type_t selected_type;

static storage_controller_type_t classify_controller(
    const pci_device_t *device
)
{
    if (
        device->class_code !=
        PCI_CLASS_STORAGE
    )
    {
        return STORAGE_CONTROLLER_NONE;
    }

    if (
        device->subclass ==
            PCI_SUBCLASS_SATA &&
        device->programming_interface ==
            PCI_PROGIF_AHCI
    )
    {
        return STORAGE_CONTROLLER_AHCI;
    }

    if (
        device->subclass ==
            PCI_SUBCLASS_NVM &&
        device->programming_interface ==
            PCI_PROGIF_NVME
    )
    {
        return STORAGE_CONTROLLER_NVME;
    }

    if (
        device->subclass ==
        PCI_SUBCLASS_IDE
    )
    {
        return STORAGE_CONTROLLER_IDE;
    }

    return STORAGE_CONTROLLER_OTHER;
}

static uint8_t controller_priority(
    storage_controller_type_t type
)
{
    switch (type)
    {
        case STORAGE_CONTROLLER_AHCI:
            return 4;

        case STORAGE_CONTROLLER_NVME:
            return 3;

        case STORAGE_CONTROLLER_IDE:
            return 2;

        case STORAGE_CONTROLLER_OTHER:
            return 1;

        default:
            return 0;
    }
}

void storage_init(void)
{
    selected_controller = NULL;
    selected_type =
        STORAGE_CONTROLLER_NONE;

    block_device_init();

    uint8_t selected_priority = 0;

    for (
        uint32_t index = 0;
        index < pci_device_count();
        index++
    )
    {
        const pci_device_t *device =
            pci_get_device(index);

        if (device == NULL)
        {
            continue;
        }

        storage_controller_type_t type =
            classify_controller(device);

        uint8_t priority =
            controller_priority(type);

        if (priority <= selected_priority)
        {
            continue;
        }

        selected_controller = device;
        selected_type = type;
        selected_priority = priority;
    }

    /*
     * Keep the legacy IDE disk first so the existing LatterOS system
     * partition remains the primary block device. AHCI disks are then
     * registered as additional modern-storage devices.
     */
    const pci_device_t *ide_controller =
        pci_find_class(
            PCI_CLASS_STORAGE,
            PCI_SUBCLASS_IDE,
            0
        );

    if (ide_controller != NULL && ata_init())
    {
        (void)block_device_register(
            ata_block_device()
        );
    }

    (void)ahci_init();
}

bool storage_controller_found(void)
{
    return selected_controller != NULL;
}

storage_controller_type_t storage_controller_type(void)
{
    return selected_type;
}

const pci_device_t *storage_controller_device(void)
{
    return selected_controller;
}

const block_device_t *storage_primary_device(void)
{
    return block_device_primary();
}

static const char *controller_name(
    storage_controller_type_t type
)
{
    switch (type)
    {
        case STORAGE_CONTROLLER_IDE:
            return "IDE controller";

        case STORAGE_CONTROLLER_AHCI:
            return "AHCI SATA controller";

        case STORAGE_CONTROLLER_NVME:
            return "NVMe controller";

        case STORAGE_CONTROLLER_OTHER:
            return "Other storage controller";

        default:
            return "No storage controller";
    }
}

static void terminal_write_hex(
    uint32_t value,
    uint8_t digits
)
{
    static const char hexadecimal[] =
        "0123456789ABCDEF";

    char text[9];

    if (digits > 8)
    {
        digits = 8;
    }

    for (
        uint8_t index = 0;
        index < digits;
        index++
    )
    {
        uint8_t shift =
            (uint8_t)(
                (digits - index - 1) * 4
            );

        text[index] =
            hexadecimal[
                (value >> shift) & 0x0F
            ];
    }

    text[digits] = '\0';

    terminal_write(text);
}

static void terminal_write_uint64(
    uint64_t value
)
{
    char temporary[21];
    char text[21];
    uint32_t length = 0;

    if (value == 0)
    {
        terminal_write("0");
        return;
    }

    while (value > 0)
    {
        temporary[length] =
            (char)('0' + value % 10);

        length++;
        value /= 10;
    }

    for (
        uint32_t index = 0;
        index < length;
        index++
    )
    {
        text[index] =
            temporary[length - index - 1];
    }

    text[length] = '\0';
    terminal_write(text);
}

static void print_location(
    const pci_device_t *device
)
{
    terminal_write_hex(
        device->bus,
        2
    );

    terminal_write(":");

    terminal_write_hex(
        device->device,
        2
    );

    terminal_write(".");

    terminal_write_hex(
        device->function,
        1
    );
}

void storage_print_controller(void)
{
    if (selected_controller == NULL)
    {
        terminal_write_line(
            "No PCI storage controller detected"
        );

        return;
    }

    terminal_write("Storage controller: ");

    terminal_write_line(
        controller_name(selected_type)
    );

    terminal_write("PCI location: ");
    print_location(selected_controller);
    terminal_write_line("");

    terminal_write("PCI ID: ");

    terminal_write_hex(
        selected_controller->vendor_id,
        4
    );

    terminal_write(":");

    terminal_write_hex(
        selected_controller->device_id,
        4
    );

    terminal_write_line("");

    terminal_write("Class: ");

    terminal_write_hex(
        selected_controller->class_code,
        2
    );

    terminal_write("/");

    terminal_write_hex(
        selected_controller->subclass,
        2
    );

    terminal_write("/");

    terminal_write_hex(
        selected_controller
            ->programming_interface,
        2
    );

    terminal_write_line("");

    if (
        selected_type ==
        STORAGE_CONTROLLER_AHCI
    )
    {
        terminal_write("AHCI driver: ");
        terminal_write_line(
            ahci_available() ?
                "active" :
                "controller found but initialization failed"
        );
    }
    else if (
        selected_type ==
        STORAGE_CONTROLLER_NVME
    )
    {
        terminal_write_line(
            "NVMe driver not active in this build"
        );
    }
}

void storage_print_devices(void)
{
    uint32_t count =
        block_device_count();

    terminal_write("Block devices: ");
    terminal_write_uint64(count);
    terminal_write_line("");

    for (
        uint32_t index = 0;
        index < count;
        index++
    )
    {
        const block_device_t *device =
            block_device_get(index);

        if (device == NULL)
        {
            continue;
        }

        terminal_write("disk");
        terminal_write_uint64(index);
        terminal_write(": ");
        terminal_write_line(device->name);

        terminal_write("  sector size: ");
        terminal_write_uint64(
            device->sector_size
        );
        terminal_write_line("");

        terminal_write("  sectors: ");
        terminal_write_uint64(
            device->sector_count
        );
        terminal_write_line("");

        terminal_write("  capacity MiB: ");
        terminal_write_uint64(
            (
                device->sector_count *
                device->sector_size
            ) /
            (1024ULL * 1024ULL)
        );
        terminal_write_line("");

        terminal_write("  writable: ");

        terminal_write_line(
            device->writable ?
            "yes" :
            "no"
        );
    }
}
