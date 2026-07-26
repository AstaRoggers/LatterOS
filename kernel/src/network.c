#include "network.h"

#include "hhdm.h"
#include "io.h"
#include "kstdio.h"
#include "memory.h"
#include "page_allocator.h"
#include "pci.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RTL8139_VENDOR_ID 0x10EC
#define RTL8139_DEVICE_ID 0x8139

#define PCI_COMMAND_IO_SPACE   0x0001
#define PCI_COMMAND_BUS_MASTER 0x0004

#define RTL8139_IDR0       0x00
#define RTL8139_TSD0       0x10
#define RTL8139_TSAD0      0x20
#define RTL8139_RBSTART    0x30
#define RTL8139_COMMAND    0x37
#define RTL8139_CAPR       0x38
#define RTL8139_IMR        0x3C
#define RTL8139_ISR        0x3E
#define RTL8139_TCR        0x40
#define RTL8139_RCR        0x44
#define RTL8139_CONFIG1    0x52
#define RTL8139_MEDIA      0x58

#define RTL8139_COMMAND_RX_BUFFER_EMPTY 0x01
#define RTL8139_COMMAND_TX_ENABLE       0x04
#define RTL8139_COMMAND_RX_ENABLE       0x08
#define RTL8139_COMMAND_RESET           0x10

#define RTL8139_ISR_RX_OK    0x0001
#define RTL8139_ISR_RX_ERROR 0x0002

#define RTL8139_RX_STATUS_OK 0x0001

#define RTL8139_TSD_OWN  (1U << 13)
#define RTL8139_TSD_TUN  (1U << 14)
#define RTL8139_TSD_TOK  (1U << 15)
#define RTL8139_TSD_TABT (1U << 30)

#define RTL8139_MEDIA_LINK_BAD (1U << 2)

#define RTL8139_RCR_ACCEPT_PHYSICAL  (1U << 1)
#define RTL8139_RCR_ACCEPT_MULTICAST (1U << 2)
#define RTL8139_RCR_ACCEPT_BROADCAST (1U << 3)
#define RTL8139_RCR_WRAP             (1U << 7)
#define RTL8139_RCR_MAX_DMA          (7U << 8)
#define RTL8139_RCR_NO_THRESHOLD     (7U << 13)

#define RTL8139_TX_SLOT_COUNT 4
#define RTL8139_TX_BUFFER_SIZE 2048
#define RTL8139_MIN_FRAME_SIZE 60
#define RTL8139_MAX_FRAME_SIZE 1514

#define RTL8139_RX_RING_SIZE       8192U
#define RTL8139_RX_EXTRA_SIZE      (16U + 1500U)
#define RTL8139_RX_ALLOCATION_SIZE 12288U
#define RTL8139_RX_PAGE_COUNT      3U

#define RTL8139_TIMEOUT 2000000U

static const pci_device_t *controller;
static uint16_t io_base;
static uint8_t mac_address[6];

static void *tx_virtual[RTL8139_TX_SLOT_COUNT];
static uint64_t tx_physical[RTL8139_TX_SLOT_COUNT];
static bool tx_used[RTL8139_TX_SLOT_COUNT];
static uint8_t tx_slot;

static uint8_t *rx_virtual;
static uint64_t rx_physical;
static uint16_t rx_offset;

static network_receive_handler_t receive_handler;

static uint64_t received_count;
static uint64_t transmitted_count;

static bool ready;

static uint16_t read_u16(
    const uint8_t *data
)
{
    return
        (uint16_t)data[0] |
        ((uint16_t)data[1] << 8);
}

static const pci_device_t *find_controller(void)
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
            device->vendor_id == RTL8139_VENDOR_ID &&
            device->device_id == RTL8139_DEVICE_ID
        )
        {
            return device;
        }
    }

    return NULL;
}

static bool reset_controller(void)
{
    outb(
        (uint16_t)(io_base + RTL8139_COMMAND),
        RTL8139_COMMAND_RESET
    );

    for (
        uint32_t timeout = 0;
        timeout < RTL8139_TIMEOUT;
        timeout++
    )
    {
        if (
            !(inb(
                (uint16_t)(io_base + RTL8139_COMMAND)
            ) & RTL8139_COMMAND_RESET)
        )
        {
            return true;
        }

        __asm__ volatile("pause");
    }

    return false;
}

static bool create_transmit_buffers(void)
{
    for (
        uint8_t index = 0;
        index < RTL8139_TX_SLOT_COUNT;
        index++
    )
    {
        void *physical_page = alloc_page();

        if (physical_page == NULL)
        {
            return false;
        }

        tx_physical[index] =
            (uint64_t)physical_page;

        tx_virtual[index] =
            physical_to_virtual(
                tx_physical[index]
            );

        tx_used[index] = false;

        memset(
            tx_virtual[index],
            0,
            RTL8139_TX_BUFFER_SIZE
        );

        outl(
            (uint16_t)(
                io_base +
                RTL8139_TSAD0 +
                index * 4
            ),
            (uint32_t)tx_physical[index]
        );
    }

    return true;
}

static bool create_receive_buffer(void)
{
    uint64_t pages[RTL8139_RX_PAGE_COUNT];

    for (
        uint8_t index = 0;
        index < RTL8139_RX_PAGE_COUNT;
        index++
    )
    {
        void *page = alloc_page();

        if (page == NULL)
        {
            for (
                uint8_t previous = 0;
                previous < index;
                previous++
            )
            {
                free_page(
                    (void *)pages[previous]
                );
            }

            return false;
        }

        pages[index] = (uint64_t)page;
    }

    uint64_t lowest = pages[0];

    for (
        uint8_t index = 1;
        index < RTL8139_RX_PAGE_COUNT;
        index++
    )
    {
        if (pages[index] < lowest)
        {
            lowest = pages[index];
        }
    }

    for (
        uint8_t expected = 0;
        expected < RTL8139_RX_PAGE_COUNT;
        expected++
    )
    {
        bool found = false;
        uint64_t address =
            lowest +
            (uint64_t)expected * 4096U;

        for (
            uint8_t index = 0;
            index < RTL8139_RX_PAGE_COUNT;
            index++
        )
        {
            if (pages[index] == address)
            {
                found = true;
                break;
            }
        }

        if (!found)
        {
            for (
                uint8_t index = 0;
                index < RTL8139_RX_PAGE_COUNT;
                index++
            )
            {
                free_page(
                    (void *)pages[index]
                );
            }

            return false;
        }
    }

    rx_physical = lowest;
    rx_virtual =
        (uint8_t *)physical_to_virtual(
            rx_physical
        );

    memset(
        rx_virtual,
        0,
        RTL8139_RX_ALLOCATION_SIZE
    );

    rx_offset = 0;

    outl(
        (uint16_t)(io_base + RTL8139_RBSTART),
        (uint32_t)rx_physical
    );

    outw(
        (uint16_t)(io_base + RTL8139_CAPR),
        0
    );

    return true;
}

bool network_init(void)
{
    controller = NULL;
    io_base = 0;
    ready = false;
    tx_slot = 0;
    rx_virtual = NULL;
    rx_physical = 0;
    rx_offset = 0;
    receive_handler = NULL;
    received_count = 0;
    transmitted_count = 0;

    for (uint8_t index = 0; index < 6; index++)
    {
        mac_address[index] = 0;
    }

    controller = find_controller();

    if (controller == NULL)
    {
        return false;
    }

    uint32_t bar = controller->bars[0];

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

    outb(
        (uint16_t)(io_base + RTL8139_CONFIG1),
        0
    );

    if (!reset_controller())
    {
        return false;
    }

    for (uint8_t index = 0; index < 6; index++)
    {
        mac_address[index] =
            inb(
                (uint16_t)(
                    io_base +
                    RTL8139_IDR0 +
                    index
                )
            );
    }

    if (
        !create_transmit_buffers() ||
        !create_receive_buffer()
    )
    {
        return false;
    }

    outw(
        (uint16_t)(io_base + RTL8139_IMR),
        0
    );

    outw(
        (uint16_t)(io_base + RTL8139_ISR),
        0xFFFF
    );

    outl(
        (uint16_t)(io_base + RTL8139_TCR),
        0x03000700U
    );

    outl(
        (uint16_t)(io_base + RTL8139_RCR),
        RTL8139_RCR_ACCEPT_PHYSICAL |
        RTL8139_RCR_ACCEPT_MULTICAST |
        RTL8139_RCR_ACCEPT_BROADCAST |
        RTL8139_RCR_WRAP |
        RTL8139_RCR_MAX_DMA |
        RTL8139_RCR_NO_THRESHOLD
    );

    outb(
        (uint16_t)(io_base + RTL8139_COMMAND),
        RTL8139_COMMAND_TX_ENABLE |
        RTL8139_COMMAND_RX_ENABLE
    );

    uint8_t command =
        inb(
            (uint16_t)(io_base + RTL8139_COMMAND)
        );

    ready =
        (command & RTL8139_COMMAND_TX_ENABLE) != 0 &&
        (command & RTL8139_COMMAND_RX_ENABLE) != 0;

    return ready;
}

bool network_is_ready(void)
{
    return ready;
}

bool network_link_up(void)
{
    if (!ready)
    {
        return false;
    }

    return
        (inb(
            (uint16_t)(io_base + RTL8139_MEDIA)
        ) & RTL8139_MEDIA_LINK_BAD) == 0;
}

const uint8_t *network_mac_address(void)
{
    return mac_address;
}

void network_set_receive_handler(
    network_receive_handler_t handler
)
{
    receive_handler = handler;
}

void network_poll(void)
{
    if (!ready)
    {
        return;
    }

    uint32_t guard = 0;

    while (
        !(inb(
            (uint16_t)(io_base + RTL8139_COMMAND)
        ) & RTL8139_COMMAND_RX_BUFFER_EMPTY) &&
        guard < 64
    )
    {
        uint8_t *entry =
            rx_virtual + rx_offset;

        uint16_t status =
            read_u16(entry);

        uint16_t packet_length =
            read_u16(entry + 2);

        if (
            !(status & RTL8139_RX_STATUS_OK) ||
            packet_length < 4 ||
            packet_length > 1522
        )
        {
            outw(
                (uint16_t)(io_base + RTL8139_ISR),
                RTL8139_ISR_RX_ERROR
            );

            rx_offset = 0;

            outw(
                (uint16_t)(io_base + RTL8139_CAPR),
                0
            );

            break;
        }

        size_t frame_length =
            (size_t)packet_length - 4U;

        if (receive_handler != NULL)
        {
            receive_handler(
                entry + 4,
                frame_length
            );
        }

        received_count++;

        uint32_t next =
            (uint32_t)rx_offset +
            (uint32_t)packet_length +
            4U;

        next =
            (next + 3U) & ~3U;

        rx_offset =
            (uint16_t)(
                next %
                RTL8139_RX_RING_SIZE
            );

        outw(
            (uint16_t)(io_base + RTL8139_CAPR),
            (uint16_t)(
                rx_offset - 16U
            )
        );

        guard++;
    }

    uint16_t interrupt_status =
        inw(
            (uint16_t)(io_base + RTL8139_ISR)
        );

    if (interrupt_status != 0)
    {
        outw(
            (uint16_t)(io_base + RTL8139_ISR),
            interrupt_status
        );
    }
}

static bool wait_for_slot(uint8_t slot)
{
    if (!tx_used[slot])
    {
        return true;
    }

    for (
        uint32_t timeout = 0;
        timeout < RTL8139_TIMEOUT;
        timeout++
    )
    {
        uint32_t status =
            inl(
                (uint16_t)(
                    io_base +
                    RTL8139_TSD0 +
                    slot * 4
                )
            );

        if (status & RTL8139_TSD_OWN)
        {
            return true;
        }

        __asm__ volatile("pause");
    }

    return false;
}

bool network_send_frame(
    const void *frame,
    size_t length
)
{
    if (
        !ready ||
        frame == NULL ||
        length > RTL8139_MAX_FRAME_SIZE
    )
    {
        return false;
    }

    size_t transmit_length = length;

    if (transmit_length < RTL8139_MIN_FRAME_SIZE)
    {
        transmit_length = RTL8139_MIN_FRAME_SIZE;
    }

    uint8_t slot = tx_slot;

    if (!wait_for_slot(slot))
    {
        return false;
    }

    memset(
        tx_virtual[slot],
        0,
        transmit_length
    );

    memcpy(
        tx_virtual[slot],
        frame,
        length
    );

    __asm__ volatile(
        ""
        :
        :
        : "memory"
    );

    outl(
        (uint16_t)(
            io_base +
            RTL8139_TSD0 +
            slot * 4
        ),
        (uint32_t)transmit_length
    );

    tx_used[slot] = true;
    tx_slot =
        (uint8_t)(
            (slot + 1) %
            RTL8139_TX_SLOT_COUNT
        );

    for (
        uint32_t timeout = 0;
        timeout < RTL8139_TIMEOUT;
        timeout++
    )
    {
        uint32_t status =
            inl(
                (uint16_t)(
                    io_base +
                    RTL8139_TSD0 +
                    slot * 4
                )
            );

        if (status & RTL8139_TSD_TOK)
        {
            transmitted_count++;
            return true;
        }

        if (
            status &
            (RTL8139_TSD_TUN |
             RTL8139_TSD_TABT)
        )
        {
            return false;
        }

        __asm__ volatile("pause");
    }

    return false;
}

bool network_send_test_frame(void)
{
    uint8_t frame[RTL8139_MIN_FRAME_SIZE];

    for (uint8_t index = 0; index < 6; index++)
    {
        frame[index] = 0xFF;
        frame[6 + index] = mac_address[index];
    }

    frame[12] = 0x88;
    frame[13] = 0xB5;

    static const char message[] =
        "LatterOS raw Ethernet test frame";

    size_t message_length =
        sizeof(message) - 1;

    for (
        size_t index = 0;
        index < message_length;
        index++
    )
    {
        frame[14 + index] =
            (uint8_t)message[index];
    }

    for (
        size_t index =
            14 + message_length;
        index < sizeof(frame);
        index++
    )
    {
        frame[index] = 0;
    }

    return network_send_frame(
        frame,
        sizeof(frame)
    );
}

uint64_t network_received_frames(void)
{
    return received_count;
}

uint64_t network_transmitted_frames(void)
{
    return transmitted_count;
}

void network_print_status(void)
{
    if (controller == NULL)
    {
        kprintf(
            "Network: no RTL8139 card detected\n"
        );
        return;
    }

    kprintf(
        "Network: RTL8139 at %02X:%02X.%u, I/O 0x%04X\n",
        (uint32_t)controller->bus,
        (uint32_t)controller->device,
        (uint32_t)controller->function,
        (uint32_t)io_base
    );

    kprintf(
        "MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
        (uint32_t)mac_address[0],
        (uint32_t)mac_address[1],
        (uint32_t)mac_address[2],
        (uint32_t)mac_address[3],
        (uint32_t)mac_address[4],
        (uint32_t)mac_address[5]
    );

    kprintf(
        "Driver: %s, link: %s, RX=%llu, TX=%llu\n",
        ready ? "ready" : "failed",
        network_link_up() ? "up" : "down",
        (unsigned long long)received_count,
        (unsigned long long)transmitted_count
    );
}
