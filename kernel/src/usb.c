#include "usb.h"

#include "hhdm.h"
#include "io.h"
#include "kstdio.h"
#include "page_allocator.h"
#include "pci.h"

#include <stddef.h>
#include <stdint.h>

#define PCI_CLASS_SERIAL_BUS 0x0C
#define PCI_SUBCLASS_USB     0x03
#define PCI_PROGIF_UHCI      0x00

#define PCI_COMMAND_IO_SPACE   0x0001
#define PCI_COMMAND_BUS_MASTER 0x0004

#define UHCI_USBCMD      0x00
#define UHCI_USBSTS      0x02
#define UHCI_USBINTR     0x04
#define UHCI_FRNUM       0x06
#define UHCI_FLBASEADD   0x08
#define UHCI_SOFMOD      0x0C
#define UHCI_PORTSC1     0x10
#define UHCI_PORTSC2     0x12

#define UHCI_CMD_RUN              0x0001
#define UHCI_CMD_HOST_RESET       0x0002
#define UHCI_CMD_CONFIGURE        0x0040
#define UHCI_CMD_MAX_PACKET_64    0x0080

#define UHCI_STATUS_CLEAR_MASK    0x003F
#define UHCI_PORT_CONNECTED       0x0001
#define UHCI_PORT_ENABLED         0x0004
#define UHCI_FRAME_TERMINATE      0x00000001U

#define UHCI_RESET_TIMEOUT        1000000U

static const pci_device_t *controller;
static uint16_t io_base;
static uint32_t *frame_list;
static uint64_t frame_list_physical;
static bool ready;

static const pci_device_t *find_uhci_controller(void)
{
    for (
        uint32_t index = 0;
        index < pci_device_count();
        index++
    )
    {
        const pci_device_t *device =
            pci_get_device(index);

        if (
            device != NULL &&
            device->class_code == PCI_CLASS_SERIAL_BUS &&
            device->subclass == PCI_SUBCLASS_USB &&
            device->programming_interface == PCI_PROGIF_UHCI
        )
        {
            return device;
        }
    }

    return NULL;
}

static bool reset_controller(void)
{
    outw(
        (uint16_t)(io_base + UHCI_USBCMD),
        UHCI_CMD_HOST_RESET
    );

    for (
        uint32_t timeout = 0;
        timeout < UHCI_RESET_TIMEOUT;
        timeout++
    )
    {
        if (
            !(inw(
                (uint16_t)(io_base + UHCI_USBCMD)
            ) & UHCI_CMD_HOST_RESET)
        )
        {
            return true;
        }

        __asm__ volatile("pause");
    }

    return false;
}

static bool create_frame_list(void)
{
    void *physical_page = alloc_page();

    if (physical_page == NULL)
    {
        return false;
    }

    frame_list_physical =
        (uint64_t)physical_page;

    frame_list =
        (uint32_t *)physical_to_virtual(
            frame_list_physical
        );

    for (
        uint32_t index = 0;
        index < 1024;
        index++
    )
    {
        frame_list[index] =
            UHCI_FRAME_TERMINATE;
    }

    return true;
}

bool usb_init(void)
{
    controller = NULL;
    io_base = 0;
    frame_list = NULL;
    frame_list_physical = 0;
    ready = false;

    controller = find_uhci_controller();

    if (controller == NULL)
    {
        return false;
    }

    uint32_t bar = controller->bars[4];

    if (!(bar & 0x01U))
    {
        return false;
    }

    io_base =
        (uint16_t)(bar & 0xFFFCU);

    if (io_base == 0)
    {
        return false;
    }

    pci_set_command_bits(
        controller,
        PCI_COMMAND_IO_SPACE |
        PCI_COMMAND_BUS_MASTER
    );

    outw(
        (uint16_t)(io_base + UHCI_USBCMD),
        0
    );

    outw(
        (uint16_t)(io_base + UHCI_USBINTR),
        0
    );

    if (!reset_controller())
    {
        return false;
    }

    if (!create_frame_list())
    {
        return false;
    }

    outw(
        (uint16_t)(io_base + UHCI_USBSTS),
        UHCI_STATUS_CLEAR_MASK
    );

    outw(
        (uint16_t)(io_base + UHCI_FRNUM),
        0
    );

    outl(
        (uint16_t)(io_base + UHCI_FLBASEADD),
        (uint32_t)frame_list_physical
    );

    outb(
        (uint16_t)(io_base + UHCI_SOFMOD),
        64
    );

    outw(
        (uint16_t)(io_base + UHCI_USBCMD),
        UHCI_CMD_RUN |
        UHCI_CMD_CONFIGURE |
        UHCI_CMD_MAX_PACKET_64
    );

    ready =
        (inw(
            (uint16_t)(io_base + UHCI_USBCMD)
        ) & UHCI_CMD_RUN) != 0;

    return ready;
}

bool usb_is_ready(void)
{
    return ready;
}

uint8_t usb_connected_port_count(void)
{
    if (!ready)
    {
        return 0;
    }

    uint8_t count = 0;

    if (
        inw(
            (uint16_t)(io_base + UHCI_PORTSC1)
        ) & UHCI_PORT_CONNECTED
    )
    {
        count++;
    }

    if (
        inw(
            (uint16_t)(io_base + UHCI_PORTSC2)
        ) & UHCI_PORT_CONNECTED
    )
    {
        count++;
    }

    return count;
}

void usb_print_status(void)
{
    if (controller == NULL)
    {
        kprintf(
            "USB: no UHCI host controller detected\n"
        );
        return;
    }

    uint16_t port1 =
        inw(
            (uint16_t)(io_base + UHCI_PORTSC1)
        );

    uint16_t port2 =
        inw(
            (uint16_t)(io_base + UHCI_PORTSC2)
        );

    kprintf(
        "USB: UHCI at %02X:%02X.%u, I/O 0x%04X\n",
        (uint32_t)controller->bus,
        (uint32_t)controller->device,
        (uint32_t)controller->function,
        (uint32_t)io_base
    );

    kprintf(
        "Controller: %s, connected ports: %u\n",
        ready ? "running" : "stopped",
        (uint32_t)usb_connected_port_count()
    );

    kprintf(
        "Port 1: %s, %s (0x%04X)\n",
        (port1 & UHCI_PORT_CONNECTED) ?
            "connected" : "empty",
        (port1 & UHCI_PORT_ENABLED) ?
            "enabled" : "disabled",
        (uint32_t)port1
    );

    kprintf(
        "Port 2: %s, %s (0x%04X)\n",
        (port2 & UHCI_PORT_CONNECTED) ?
            "connected" : "empty",
        (port2 & UHCI_PORT_ENABLED) ?
            "enabled" : "disabled",
        (uint32_t)port2
    );
}
