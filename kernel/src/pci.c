#include "pci.h"

#include "io.h"
#include "terminal.h"

#include <stddef.h>
#include <stdint.h>

#define PCI_CONFIG_ADDRESS_PORT 0xCF8
#define PCI_CONFIG_DATA_PORT    0xCFC

#define PCI_VENDOR_NONE         0xFFFF
#define PCI_HEADER_MULTIFUNCTION 0x80
#define PCI_HEADER_TYPE_MASK     0x7F

#define PCI_HEADER_DEVICE        0x00
#define PCI_HEADER_BRIDGE        0x01

static pci_device_t devices[PCI_MAX_DEVICES];
static uint32_t device_count;

static uint32_t pci_config_address(
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint8_t offset
)
{
    return
        0x80000000U |
        ((uint32_t)bus << 16) |
        ((uint32_t)device << 11) |
        ((uint32_t)function << 8) |
        ((uint32_t)offset & 0xFCU);
}

uint32_t pci_config_read32(
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint8_t offset
)
{
    outl(
        PCI_CONFIG_ADDRESS_PORT,
        pci_config_address(
            bus,
            device,
            function,
            offset
        )
    );

    return inl(PCI_CONFIG_DATA_PORT);
}

void pci_config_write32(
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint8_t offset,
    uint32_t value
)
{
    outl(
        PCI_CONFIG_ADDRESS_PORT,
        pci_config_address(
            bus,
            device,
            function,
            offset
        )
    );

    outl(
        PCI_CONFIG_DATA_PORT,
        value
    );
}

void pci_config_write16(
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint8_t offset,
    uint16_t value
)
{
    uint8_t aligned_offset =
        (uint8_t)(offset & 0xFCU);

    uint32_t current =
        pci_config_read32(
            bus,
            device,
            function,
            aligned_offset
        );

    uint8_t shift =
        (uint8_t)((offset & 2U) * 8U);

    uint32_t mask =
        0xFFFFU << shift;

    uint32_t updated =
        (current & ~mask) |
        ((uint32_t)value << shift);

    pci_config_write32(
        bus,
        device,
        function,
        aligned_offset,
        updated
    );
}

void pci_set_command_bits(
    const pci_device_t *device,
    uint16_t bits
)
{
    if (device == NULL)
    {
        return;
    }

    uint16_t command =
        pci_config_read16(
            device->bus,
            device->device,
            device->function,
            0x04
        );

    command |= bits;

    pci_config_write16(
        device->bus,
        device->device,
        device->function,
        0x04,
        command
    );
}

uint16_t pci_config_read16(
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint8_t offset
)
{
    uint32_t value =
        pci_config_read32(
            bus,
            device,
            function,
            offset
        );

    uint8_t shift =
        (uint8_t)((offset & 2U) * 8U);

    return (uint16_t)(
        (value >> shift) & 0xFFFFU
    );
}

uint8_t pci_config_read8(
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint8_t offset
)
{
    uint32_t value =
        pci_config_read32(
            bus,
            device,
            function,
            offset
        );

    uint8_t shift =
        (uint8_t)((offset & 3U) * 8U);

    return (uint8_t)(
        (value >> shift) & 0xFFU
    );
}

static void pci_store_function(
    uint8_t bus,
    uint8_t device,
    uint8_t function
)
{
    if (device_count >= PCI_MAX_DEVICES)
    {
        return;
    }

    uint16_t vendor_id =
        pci_config_read16(
            bus,
            device,
            function,
            0x00
        );

    if (vendor_id == PCI_VENDOR_NONE)
    {
        return;
    }

    pci_device_t *entry =
        &devices[device_count];

    entry->bus = bus;
    entry->device = device;
    entry->function = function;

    entry->vendor_id = vendor_id;

    entry->device_id =
        pci_config_read16(
            bus,
            device,
            function,
            0x02
        );

    entry->revision_id =
        pci_config_read8(
            bus,
            device,
            function,
            0x08
        );

    entry->programming_interface =
        pci_config_read8(
            bus,
            device,
            function,
            0x09
        );

    entry->subclass =
        pci_config_read8(
            bus,
            device,
            function,
            0x0A
        );

    entry->class_code =
        pci_config_read8(
            bus,
            device,
            function,
            0x0B
        );

    entry->header_type =
        pci_config_read8(
            bus,
            device,
            function,
            0x0E
        );

    for (uint8_t index = 0; index < 6; index++)
    {
        entry->bars[index] = 0;
    }

    uint8_t basic_header =
        entry->header_type &
        PCI_HEADER_TYPE_MASK;

    uint8_t bar_count = 0;

    if (basic_header == PCI_HEADER_DEVICE)
    {
        bar_count = 6;
    }
    else if (basic_header == PCI_HEADER_BRIDGE)
    {
        bar_count = 2;
    }

    for (
        uint8_t index = 0;
        index < bar_count;
        index++
    )
    {
        entry->bars[index] =
            pci_config_read32(
                bus,
                device,
                function,
                (uint8_t)(
                    0x10 + index * 4
                )
            );
    }

    device_count++;
}

void pci_init(void)
{
    device_count = 0;

    for (
        uint16_t bus = 0;
        bus < 256;
        bus++
    )
    {
        for (
            uint8_t device = 0;
            device < 32;
            device++
        )
        {
            uint16_t vendor_id =
                pci_config_read16(
                    (uint8_t)bus,
                    device,
                    0,
                    0x00
                );

            if (vendor_id == PCI_VENDOR_NONE)
            {
                continue;
            }

            pci_store_function(
                (uint8_t)bus,
                device,
                0
            );

            uint8_t header_type =
                pci_config_read8(
                    (uint8_t)bus,
                    device,
                    0,
                    0x0E
                );

            if (
                !(header_type &
                PCI_HEADER_MULTIFUNCTION)
            )
            {
                continue;
            }

            for (
                uint8_t function = 1;
                function < 8;
                function++
            )
            {
                pci_store_function(
                    (uint8_t)bus,
                    device,
                    function
                );
            }
        }
    }
}

uint32_t pci_device_count(void)
{
    return device_count;
}

const pci_device_t *pci_get_device(
    uint32_t index
)
{
    if (index >= device_count)
    {
        return NULL;
    }

    return &devices[index];
}

const pci_device_t *pci_find_class(
    uint8_t class_code,
    uint8_t subclass,
    uint32_t occurrence
)
{
    uint32_t match = 0;

    for (
        uint32_t index = 0;
        index < device_count;
        index++
    )
    {
        if (
            devices[index].class_code !=
                class_code ||
            devices[index].subclass !=
                subclass
        )
        {
            continue;
        }

        if (match == occurrence)
        {
            return &devices[index];
        }

        match++;
    }

    return NULL;
}

static const char *pci_class_name(
    uint8_t class_code
)
{
    switch (class_code)
    {
        case 0x01:
            return "Storage";

        case 0x02:
            return "Network";

        case 0x03:
            return "Display";

        case 0x04:
            return "Multimedia";

        case 0x05:
            return "Memory";

        case 0x06:
            return "Bridge";

        case 0x07:
            return "Communication";

        case 0x08:
            return "System";

        case 0x09:
            return "Input";

        case 0x0C:
            return "Serial bus";

        default:
            return "Other";
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

static void terminal_write_decimal(
    uint32_t value
)
{
    char reverse[10];
    char text[11];
    uint8_t length = 0;

    if (value == 0)
    {
        terminal_write("0");
        return;
    }

    while (value > 0)
    {
        reverse[length] =
            (char)(
                '0' + value % 10
            );

        length++;
        value /= 10;
    }

    for (
        uint8_t index = 0;
        index < length;
        index++
    )
    {
        text[index] =
            reverse[length - index - 1];
    }

    text[length] = '\0';

    terminal_write(text);
}

void pci_print_devices(void)
{
    terminal_write("PCI devices: ");
    terminal_write_decimal(device_count);
    terminal_write_line("");

    if (device_count == 0)
    {
        terminal_write_line(
            "No PCI devices detected"
        );

        return;
    }

    for (
        uint32_t index = 0;
        index < device_count;
        index++
    )
    {
        const pci_device_t *entry =
            &devices[index];

        terminal_write_hex(
            entry->bus,
            2
        );

        terminal_write(":");

        terminal_write_hex(
            entry->device,
            2
        );

        terminal_write(".");

        terminal_write_hex(
            entry->function,
            1
        );

        terminal_write(" ");

        terminal_write_hex(
            entry->vendor_id,
            4
        );

        terminal_write(":");

        terminal_write_hex(
            entry->device_id,
            4
        );

        terminal_write(" ");

        terminal_write(
            pci_class_name(
                entry->class_code
            )
        );

        terminal_write(" ");

        terminal_write_hex(
            entry->class_code,
            2
        );

        terminal_write("/");

        terminal_write_hex(
            entry->subclass,
            2
        );

        terminal_write("/");

        terminal_write_hex(
            entry->programming_interface,
            2
        );

        terminal_write_line("");
    }
}
