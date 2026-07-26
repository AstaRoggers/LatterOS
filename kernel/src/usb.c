#include "usb.h"

#include "hhdm.h"
#include "io.h"
#include "keyboard.h"
#include "kstdio.h"
#include "mouse.h"
#include "page_allocator.h"
#include "pci.h"
#include "physical_memory.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PCI_CLASS_SERIAL_BUS 0x0C
#define PCI_SUBCLASS_USB     0x03
#define PCI_PROGIF_UHCI      0x00

#define UHCI_ROOT_PORT_COUNT 2

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
#define UHCI_PORT_CONNECT_CHANGE  0x0002
#define UHCI_PORT_ENABLED         0x0004
#define UHCI_PORT_ENABLE_CHANGE   0x0008
#define UHCI_PORT_LOW_SPEED       0x0100
#define UHCI_PORT_RESET           0x0200

#define UHCI_LINK_TERMINATE       0x00000001U
#define UHCI_LINK_QH              0x00000002U
#define UHCI_LINK_DEPTH_FIRST     0x00000004U

#define UHCI_TD_ACTIVE            (1U << 23)
#define UHCI_TD_IOC               (1U << 24)
#define UHCI_TD_LOW_SPEED         (1U << 26)
#define UHCI_TD_ERROR_COUNT_3     (3U << 27)
#define UHCI_TD_SHORT_PACKET      (1U << 29)
#define UHCI_TD_ERROR_MASK        0x007F0000U

#define UHCI_PID_OUT              0xE1U
#define UHCI_PID_IN               0x69U
#define UHCI_PID_SETUP            0x2DU

#define USB_REQUEST_GET_DESCRIPTOR 0x06
#define USB_REQUEST_SET_ADDRESS    0x05
#define USB_REQUEST_SET_CONFIG     0x09
#define USB_REQUEST_SET_IDLE       0x0A
#define USB_REQUEST_SET_PROTOCOL   0x0B
#define USB_REQUEST_GET_STATUS     0x00
#define USB_REQUEST_CLEAR_FEATURE  0x01
#define USB_REQUEST_SET_FEATURE    0x03

#define USB_DESCRIPTOR_DEVICE       0x01
#define USB_DESCRIPTOR_CONFIGURATION 0x02
#define USB_DESCRIPTOR_INTERFACE    0x04
#define USB_DESCRIPTOR_ENDPOINT     0x05
#define USB_DESCRIPTOR_HUB          0x29

#define USB_CLASS_HID                0x03
#define USB_HID_SUBCLASS_BOOT        0x01
#define USB_HID_PROTOCOL_KEYBOARD    0x01
#define USB_HID_PROTOCOL_MOUSE       0x02
#define USB_ENDPOINT_DIRECTION_IN    0x80
#define USB_ENDPOINT_TRANSFER_MASK   0x03
#define USB_ENDPOINT_BULK            0x02
#define USB_ENDPOINT_INTERRUPT       0x03
#define USB_REQUEST_TYPE_CLASS_OUT_INTERFACE 0x21

#define USB_CLASS_MASS_STORAGE       0x08
#define USB_MASS_SUBCLASS_SCSI       0x06
#define USB_MASS_PROTOCOL_BULK_ONLY  0x50

#define USB_REQUEST_TYPE_DEVICE_IN  0x80
#define USB_REQUEST_TYPE_DEVICE_OUT 0x00
#define USB_REQUEST_TYPE_HUB_IN     0xA0
#define USB_REQUEST_TYPE_HUB_PORT_IN  0xA3
#define USB_REQUEST_TYPE_HUB_PORT_OUT 0x23

#define USB_CLASS_HUB               0x09
#define USB_HUB_PORT_CONNECTION     0x0001
#define USB_HUB_PORT_ENABLE         0x0002
#define USB_HUB_PORT_RESET          0x0010
#define USB_HUB_PORT_POWER          0x0100
#define USB_HUB_PORT_LOW_SPEED      0x0200
#define USB_HUB_CHANGE_CONNECTION   0x0001
#define USB_HUB_CHANGE_ENABLE       0x0002
#define USB_HUB_CHANGE_RESET        0x0010
#define USB_HUB_FEATURE_PORT_RESET  4
#define USB_HUB_FEATURE_PORT_POWER  8
#define USB_HUB_FEATURE_C_CONNECTION 16
#define USB_HUB_FEATURE_C_ENABLE    17
#define USB_HUB_FEATURE_C_RESET     20
#define USB_MAX_HUB_DEPTH           3

#define USB_ENUMERATION_ADDRESS     1

#define UHCI_RESET_TIMEOUT          1000000U
#define UHCI_TRANSFER_TIMEOUT       5000000U
#define USB_PORT_DELAY_LOOPS        250000U
#define UHCI_MAX_TDS                40
#define USB_MAX_INTERRUPT_PACKET    8

/*
 * UHCI queue heads and transfer descriptors are 16-byte aligned and
 * are addressed by their physical DMA addresses.
 */
typedef struct __attribute__((aligned(16)))
{
    volatile uint32_t head_link;
    volatile uint32_t element_link;
    uint32_t reserved[2];
} uhci_queue_head_t;

typedef struct __attribute__((aligned(16)))
{
    volatile uint32_t link;
    volatile uint32_t control_status;
    volatile uint32_t token;
    volatile uint32_t buffer;
} uhci_transfer_descriptor_t;

typedef struct __attribute__((packed))
{
    uint8_t request_type;
    uint8_t request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
} usb_setup_packet_t;

typedef struct __attribute__((packed))
{
    uint8_t length;
    uint8_t descriptor_type;
    uint16_t usb_version;
    uint8_t device_class;
    uint8_t device_subclass;
    uint8_t device_protocol;
    uint8_t max_packet_size;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t device_version;
    uint8_t manufacturer_string;
    uint8_t product_string;
    uint8_t serial_string;
    uint8_t configuration_count;
} usb_device_descriptor_t;

typedef struct __attribute__((packed))
{
    uint8_t length;
    uint8_t descriptor_type;
    uint16_t total_length;
    uint8_t interface_count;
    uint8_t configuration_value;
    uint8_t configuration_string;
    uint8_t attributes;
    uint8_t maximum_power;
} usb_configuration_descriptor_t;

typedef struct __attribute__((packed))
{
    uint8_t length;
    uint8_t descriptor_type;
    uint8_t interface_number;
    uint8_t alternate_setting;
    uint8_t endpoint_count;
    uint8_t interface_class;
    uint8_t interface_subclass;
    uint8_t interface_protocol;
    uint8_t interface_string;
} usb_interface_descriptor_t;


typedef struct __attribute__((packed))
{
    uint8_t length;
    uint8_t descriptor_type;
    uint8_t endpoint_address;
    uint8_t attributes;
    uint16_t maximum_packet_size;
    uint8_t interval;
} usb_endpoint_descriptor_t;

typedef struct __attribute__((packed))
{
    uint8_t length;
    uint8_t descriptor_type;
    uint8_t port_count;
    uint16_t characteristics;
    uint8_t power_on_to_power_good;
    uint8_t controller_current;
} usb_hub_descriptor_t;

typedef struct __attribute__((packed))
{
    uint16_t status;
    uint16_t change;
} usb_hub_port_status_t;

typedef struct
{
    bool active;
    uint8_t device_index;
    bool data_toggle;
    uint8_t packet_length;

    uhci_queue_head_t *queue_head;
    uint64_t queue_head_physical;

    uhci_transfer_descriptor_t *descriptor;
    uint64_t descriptor_physical;

    uint8_t *report_buffer;
    uint64_t report_buffer_physical;
} usb_interrupt_pipe_t;

static const pci_device_t *controller;
static uint16_t io_base;
static uint32_t *frame_list;
static uint64_t frame_list_physical;
static uhci_queue_head_t *control_qh;
static uint64_t control_qh_physical;
static uhci_transfer_descriptor_t *transfer_descriptors;
static uint64_t transfer_descriptors_physical;
static usb_setup_packet_t *setup_packet;
static uint64_t setup_packet_physical;
static uint8_t *transfer_buffer;
static uint64_t transfer_buffer_physical;

static uint8_t *interrupt_area;
static uint64_t interrupt_area_physical;
static usb_interrupt_pipe_t interrupt_pipes[USB_MAX_DEVICES];

static bool ready;
static usb_device_t devices[USB_MAX_DEVICES];
static uint8_t enumerated_device_count;
static bool keyboard_ready;
static bool mouse_ready;

static void memory_clear(
    void *pointer,
    size_t count
)
{
    uint8_t *bytes = pointer;

    for (
        size_t index = 0;
        index < count;
        index++
    )
    {
        bytes[index] = 0;
    }
}

static void memory_copy(
    void *destination,
    const void *source,
    size_t count
)
{
    uint8_t *output = destination;
    const uint8_t *input = source;

    for (
        size_t index = 0;
        index < count;
        index++
    )
    {
        output[index] = input[index];
    }
}

static void short_delay(void)
{
    for (
        uint32_t count = 0;
        count < USB_PORT_DELAY_LOOPS;
        count++
    )
    {
        __asm__ volatile("pause");
    }
}

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

static uint16_t port_register(uint8_t port)
{
    return (uint16_t)(
        io_base +
        (port == 1 ? UHCI_PORTSC1 : UHCI_PORTSC2)
    );
}

static void port_clear_changes(uint8_t port)
{
    uint16_t register_port =
        port_register(port);

    uint16_t value = inw(register_port);

    outw(
        register_port,
        (uint16_t)(
            value |
            UHCI_PORT_CONNECT_CHANGE |
            UHCI_PORT_ENABLE_CHANGE
        )
    );
}

static bool reset_port(
    uint8_t port,
    bool *low_speed
)
{
    uint16_t register_port =
        port_register(port);

    uint16_t status = inw(register_port);

    if (!(status & UHCI_PORT_CONNECTED))
    {
        return false;
    }

    port_clear_changes(port);

    status = inw(register_port);

    outw(
        register_port,
        (uint16_t)(status | UHCI_PORT_RESET)
    );

    short_delay();
    short_delay();

    status = inw(register_port);

    outw(
        register_port,
        (uint16_t)(
            (status & ~UHCI_PORT_RESET) |
            UHCI_PORT_CONNECT_CHANGE |
            UHCI_PORT_ENABLE_CHANGE
        )
    );

    short_delay();

    for (
        uint32_t attempt = 0;
        attempt < 100;
        attempt++
    )
    {
        status = inw(register_port);

        if (!(status & UHCI_PORT_CONNECTED))
        {
            return false;
        }

        if (status & UHCI_PORT_ENABLED)
        {
            if (low_speed != NULL)
            {
                *low_speed =
                    (status & UHCI_PORT_LOW_SPEED) != 0;
            }

            port_clear_changes(port);
            return true;
        }

        outw(
            register_port,
            (uint16_t)(
                status |
                UHCI_PORT_ENABLED |
                UHCI_PORT_CONNECT_CHANGE |
                UHCI_PORT_ENABLE_CHANGE
            )
        );

        short_delay();
    }

    return false;
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

static bool create_dma_area(void)
{
    void *frame_page = alloc_page();
    void *control_page = alloc_page();
    void *interrupt_page = alloc_page();

    if (
        frame_page == NULL ||
        control_page == NULL ||
        interrupt_page == NULL
    )
    {
        if (frame_page != NULL)
        {
            free_page(frame_page);
        }

        if (control_page != NULL)
        {
            free_page(control_page);
        }

        if (interrupt_page != NULL)
        {
            free_page(interrupt_page);
        }

        return false;
    }

    frame_list_physical =
        (uint64_t)frame_page;

    frame_list =
        (uint32_t *)physical_to_virtual(
            frame_list_physical
        );

    uint8_t *control_virtual =
        (uint8_t *)physical_to_virtual(
            (uint64_t)control_page
        );

    interrupt_area_physical =
        (uint64_t)interrupt_page;

    interrupt_area =
        (uint8_t *)physical_to_virtual(
            interrupt_area_physical
        );

    memory_clear(
        frame_list,
        PAGE_SIZE
    );

    memory_clear(
        control_virtual,
        PAGE_SIZE
    );

    memory_clear(
        interrupt_area,
        PAGE_SIZE
    );

    control_qh =
        (uhci_queue_head_t *)control_virtual;

    control_qh_physical =
        (uint64_t)control_page;

    transfer_descriptors =
        (uhci_transfer_descriptor_t *)(
            control_virtual + 0x20
        );

    transfer_descriptors_physical =
        (uint64_t)control_page + 0x20;

    setup_packet =
        (usb_setup_packet_t *)(
            control_virtual + 0x300
        );

    setup_packet_physical =
        (uint64_t)control_page + 0x300;

    transfer_buffer =
        control_virtual + 0x400;

    transfer_buffer_physical =
        (uint64_t)control_page + 0x400;

    for (
        uint32_t index = 0;
        index < 1024;
        index++
    )
    {
        frame_list[index] =
            (uint32_t)(
                control_qh_physical |
                UHCI_LINK_QH
            );
    }

    control_qh->head_link =
        UHCI_LINK_TERMINATE;

    control_qh->element_link =
        UHCI_LINK_TERMINATE;

    return true;
}

static uint32_t td_token(
    uint8_t pid,
    uint8_t address,
    uint8_t endpoint,
    bool data_toggle,
    uint16_t length
)
{
    uint32_t encoded_length =
        length == 0 ?
            0x7FFU :
            (uint32_t)(length - 1);

    return
        (uint32_t)pid |
        ((uint32_t)(address & 0x7FU) << 8) |
        ((uint32_t)(endpoint & 0x0FU) << 15) |
        ((uint32_t)(data_toggle ? 1U : 0U) << 19) |
        ((encoded_length & 0x7FFU) << 21);
}

static void initialize_td(
    uint32_t index,
    uint32_t next_index,
    bool has_next,
    uint8_t pid,
    uint8_t address,
    uint8_t endpoint,
    bool data_toggle,
    uint16_t length,
    uint64_t buffer_physical,
    bool low_speed,
    bool interrupt_on_complete
)
{
    uhci_transfer_descriptor_t *td =
        &transfer_descriptors[index];

    td->link = has_next ?
        (uint32_t)(
            transfer_descriptors_physical +
            (uint64_t)next_index *
                sizeof(uhci_transfer_descriptor_t) |
            UHCI_LINK_DEPTH_FIRST
        ) :
        UHCI_LINK_TERMINATE;

    td->control_status =
        UHCI_TD_ACTIVE |
        UHCI_TD_ERROR_COUNT_3 |
        (low_speed ? UHCI_TD_LOW_SPEED : 0U) |
        (pid == UHCI_PID_IN ?
            UHCI_TD_SHORT_PACKET : 0U) |
        (interrupt_on_complete ?
            UHCI_TD_IOC : 0U);

    td->token = td_token(
        pid,
        address,
        endpoint,
        data_toggle,
        length
    );

    td->buffer =
        length == 0 ?
            0 :
            (uint32_t)buffer_physical;
}

static bool wait_for_transfer(
    uint32_t td_count
)
{
    if (td_count == 0)
    {
        return false;
    }

    uhci_transfer_descriptor_t *last =
        &transfer_descriptors[td_count - 1];

    for (
        uint32_t timeout = 0;
        timeout < UHCI_TRANSFER_TIMEOUT;
        timeout++
    )
    {
        uint32_t status =
            last->control_status;

        if (!(status & UHCI_TD_ACTIVE))
        {
            break;
        }

        __asm__ volatile("pause");
    }

    if (last->control_status & UHCI_TD_ACTIVE)
    {
        return false;
    }

    for (
        uint32_t index = 0;
        index < td_count;
        index++
    )
    {
        uint32_t status =
            transfer_descriptors[index]
                .control_status;

        if (
            (status & UHCI_TD_ACTIVE) ||
            (status & UHCI_TD_ERROR_MASK)
        )
        {
            return false;
        }
    }

    return true;
}

static bool control_transfer(
    uint8_t address,
    uint8_t request_type,
    uint8_t request,
    uint16_t value,
    uint16_t index,
    void *data,
    uint16_t length,
    uint8_t max_packet_size,
    bool low_speed
)
{
    if (
        !ready ||
        max_packet_size == 0 ||
        length >
            (PAGE_SIZE - 0x400)
    )
    {
        return false;
    }

    memory_clear(
        transfer_descriptors,
        UHCI_MAX_TDS *
            sizeof(uhci_transfer_descriptor_t)
    );

    memory_clear(
        transfer_buffer,
        length
    );

    setup_packet->request_type =
        request_type;
    setup_packet->request = request;
    setup_packet->value = value;
    setup_packet->index = index;
    setup_packet->length = length;

    if (
        length > 0 &&
        !(request_type & 0x80U) &&
        data != NULL
    )
    {
        memory_copy(
            transfer_buffer,
            data,
            length
        );
    }

    uint32_t td_count = 0;

    initialize_td(
        td_count,
        td_count + 1,
        true,
        UHCI_PID_SETUP,
        address,
        0,
        false,
        sizeof(usb_setup_packet_t),
        setup_packet_physical,
        low_speed,
        false
    );

    td_count++;

    bool data_in =
        (request_type & 0x80U) != 0;

    uint16_t transferred = 0;
    bool toggle = true;

    while (transferred < length)
    {
        uint16_t packet_length =
            (uint16_t)(
                length - transferred
            );

        if (packet_length > max_packet_size)
        {
            packet_length = max_packet_size;
        }

        if (td_count + 1 >= UHCI_MAX_TDS)
        {
            return false;
        }

        initialize_td(
            td_count,
            td_count + 1,
            true,
            data_in ?
                UHCI_PID_IN :
                UHCI_PID_OUT,
            address,
            0,
            toggle,
            packet_length,
            transfer_buffer_physical +
                transferred,
            low_speed,
            false
        );

        toggle = !toggle;
        transferred =
            (uint16_t)(
                transferred + packet_length
            );

        td_count++;
    }

    initialize_td(
        td_count,
        0,
        false,
        data_in ?
            UHCI_PID_OUT :
            UHCI_PID_IN,
        address,
        0,
        true,
        0,
        0,
        low_speed,
        true
    );

    td_count++;

    __asm__ volatile(
        "mfence"
        :
        :
        : "memory"
    );

    control_qh->element_link =
        (uint32_t)transfer_descriptors_physical;

    __asm__ volatile(
        "mfence"
        :
        :
        : "memory"
    );

    bool success =
        wait_for_transfer(td_count);

    control_qh->element_link =
        UHCI_LINK_TERMINATE;

    __asm__ volatile(
        "mfence"
        :
        :
        : "memory"
    );

    if (
        success &&
        length > 0 &&
        data_in &&
        data != NULL
    )
    {
        memory_copy(
            data,
            transfer_buffer,
            length
        );
    }

    return success;
}

static bool bulk_transfer(
    const usb_device_t *device,
    uint8_t endpoint,
    bool direction_in,
    uint16_t packet_size,
    bool *data_toggle,
    void *data,
    uint16_t length
)
{
    if (
        !ready ||
        device == NULL ||
        endpoint == 0 ||
        packet_size == 0 ||
        data_toggle == NULL ||
        data == NULL ||
        length == 0 ||
        length > (PAGE_SIZE - 0x400)
    )
    {
        return false;
    }

    memory_clear(
        transfer_descriptors,
        UHCI_MAX_TDS *
            sizeof(uhci_transfer_descriptor_t)
    );

    if (direction_in)
    {
        memory_clear(transfer_buffer, length);
    }
    else
    {
        memory_copy(
            transfer_buffer,
            data,
            length
        );
    }

    uint16_t transferred = 0;
    uint32_t td_count = 0;
    bool toggle = *data_toggle;

    while (transferred < length)
    {
        uint16_t packet_length =
            (uint16_t)(length - transferred);

        if (packet_length > packet_size)
        {
            packet_length = packet_size;
        }

        if (td_count >= UHCI_MAX_TDS)
        {
            return false;
        }

        bool final_packet =
            (uint16_t)(transferred + packet_length) ==
            length;

        initialize_td(
            td_count,
            td_count + 1,
            !final_packet,
            direction_in ?
                UHCI_PID_IN :
                UHCI_PID_OUT,
            device->address,
            endpoint,
            toggle,
            packet_length,
            transfer_buffer_physical +
                transferred,
            device->low_speed,
            final_packet
        );

        toggle = !toggle;
        transferred =
            (uint16_t)(transferred + packet_length);
        td_count++;
    }

    __asm__ volatile(
        "mfence"
        :
        :
        : "memory"
    );

    control_qh->element_link =
        (uint32_t)transfer_descriptors_physical;

    __asm__ volatile(
        "mfence"
        :
        :
        : "memory"
    );

    bool success =
        wait_for_transfer(td_count);

    control_qh->element_link =
        UHCI_LINK_TERMINATE;

    __asm__ volatile(
        "mfence"
        :
        :
        : "memory"
    );

    if (!success)
    {
        return false;
    }

    if (direction_in)
    {
        memory_copy(
            data,
            transfer_buffer,
            length
        );
    }

    *data_toggle = toggle;
    return true;
}

static bool get_descriptor(
    uint8_t address,
    uint8_t descriptor_type,
    uint8_t descriptor_index,
    void *buffer,
    uint16_t length,
    uint8_t max_packet_size,
    bool low_speed
)
{
    return control_transfer(
        address,
        USB_REQUEST_TYPE_DEVICE_IN,
        USB_REQUEST_GET_DESCRIPTOR,
        (uint16_t)(
            (uint16_t)descriptor_type << 8 |
            descriptor_index
        ),
        0,
        buffer,
        length,
        max_packet_size,
        low_speed
    );
}

static bool set_address(
    uint8_t new_address,
    uint8_t max_packet_size,
    bool low_speed
)
{
    return control_transfer(
        0,
        USB_REQUEST_TYPE_DEVICE_OUT,
        USB_REQUEST_SET_ADDRESS,
        new_address,
        0,
        NULL,
        0,
        max_packet_size,
        low_speed
    );
}

static bool set_configuration(
    uint8_t address,
    uint8_t configuration,
    uint8_t max_packet_size,
    bool low_speed
)
{
    return control_transfer(
        address,
        USB_REQUEST_TYPE_DEVICE_OUT,
        USB_REQUEST_SET_CONFIG,
        configuration,
        0,
        NULL,
        0,
        max_packet_size,
        low_speed
    );
}


static bool set_hid_protocol(
    const usb_device_t *device
)
{
    if (device == NULL)
    {
        return false;
    }

    return control_transfer(
        device->address,
        USB_REQUEST_TYPE_CLASS_OUT_INTERFACE,
        USB_REQUEST_SET_PROTOCOL,
        0,
        device->interface_number,
        NULL,
        0,
        device->max_packet_size,
        device->low_speed
    );
}

static bool set_hid_idle(
    const usb_device_t *device
)
{
    if (device == NULL)
    {
        return false;
    }

    return control_transfer(
        device->address,
        USB_REQUEST_TYPE_CLASS_OUT_INTERFACE,
        USB_REQUEST_SET_IDLE,
        0,
        device->interface_number,
        NULL,
        0,
        device->max_packet_size,
        device->low_speed
    );
}

static void parse_configuration(
    usb_device_t *device,
    const uint8_t *bytes,
    uint16_t length
)
{
    if (
        device == NULL ||
        bytes == NULL ||
        length <
            sizeof(usb_configuration_descriptor_t)
    )
    {
        return;
    }

    const usb_configuration_descriptor_t *configuration =
        (const usb_configuration_descriptor_t *)bytes;

    device->configuration_value =
        configuration->configuration_value;

    device->interface_count =
        configuration->interface_count;

    bool found_interface = false;
    bool found_preferred = false;
    bool current_hid = false;
    bool current_mass_storage = false;

    uint16_t offset = 0;

    while (offset + 2 <= length)
    {
        uint8_t descriptor_length =
            bytes[offset];

        uint8_t descriptor_type =
            bytes[offset + 1];

        if (
            descriptor_length < 2 ||
            offset + descriptor_length > length
        )
        {
            break;
        }

        if (
            descriptor_type ==
                USB_DESCRIPTOR_INTERFACE &&
            descriptor_length >=
                sizeof(usb_interface_descriptor_t)
        )
        {
            const usb_interface_descriptor_t *interface =
                (const usb_interface_descriptor_t *)(
                    &bytes[offset]
                );

            current_hid =
                interface->interface_class ==
                    USB_CLASS_HID &&
                interface->interface_subclass ==
                    USB_HID_SUBCLASS_BOOT &&
                (
                    interface->interface_protocol ==
                        USB_HID_PROTOCOL_KEYBOARD ||
                    interface->interface_protocol ==
                        USB_HID_PROTOCOL_MOUSE
                );

            current_mass_storage =
                interface->interface_class ==
                    USB_CLASS_MASS_STORAGE &&
                interface->interface_subclass ==
                    USB_MASS_SUBCLASS_SCSI &&
                interface->interface_protocol ==
                    USB_MASS_PROTOCOL_BULK_ONLY;

            bool preferred =
                current_hid ||
                current_mass_storage;

            if (
                !found_interface ||
                (preferred && !found_preferred)
            )
            {
                device->interface_number =
                    interface->interface_number;

                device->endpoint_count =
                    interface->endpoint_count;

                device->interface_class =
                    interface->interface_class;

                device->interface_subclass =
                    interface->interface_subclass;

                device->interface_protocol =
                    interface->interface_protocol;

                found_interface = true;

                if (preferred)
                {
                    found_preferred = true;
                }
            }

            if (current_hid)
            {
                device->hid_boot_keyboard =
                    interface->interface_protocol ==
                        USB_HID_PROTOCOL_KEYBOARD;

                device->hid_boot_mouse =
                    interface->interface_protocol ==
                        USB_HID_PROTOCOL_MOUSE;
            }

            if (current_mass_storage)
            {
                device->mass_storage = true;
                device->interface_number =
                    interface->interface_number;
                device->endpoint_count =
                    interface->endpoint_count;
                device->interface_class =
                    interface->interface_class;
                device->interface_subclass =
                    interface->interface_subclass;
                device->interface_protocol =
                    interface->interface_protocol;
            }
        }
        else if (
            descriptor_type ==
                USB_DESCRIPTOR_ENDPOINT &&
            descriptor_length >=
                sizeof(usb_endpoint_descriptor_t)
        )
        {
            const usb_endpoint_descriptor_t *endpoint =
                (const usb_endpoint_descriptor_t *)(
                    &bytes[offset]
                );

            uint8_t transfer_type =
                (uint8_t)(
                    endpoint->attributes &
                    USB_ENDPOINT_TRANSFER_MASK
                );

            uint16_t packet_size =
                (uint16_t)(
                    endpoint->maximum_packet_size &
                    0x07FFU
                );

            if (
                current_hid &&
                device->interrupt_endpoint == 0 &&
                (endpoint->endpoint_address &
                    USB_ENDPOINT_DIRECTION_IN) &&
                transfer_type ==
                    USB_ENDPOINT_INTERRUPT
            )
            {
                device->interrupt_endpoint =
                    (uint8_t)(
                        endpoint->endpoint_address &
                        0x0FU
                    );

                device->interrupt_packet_size =
                    packet_size;

                device->interrupt_interval =
                    endpoint->interval;
            }

            if (
                current_mass_storage &&
                transfer_type == USB_ENDPOINT_BULK
            )
            {
                uint8_t endpoint_number =
                    (uint8_t)(
                        endpoint->endpoint_address &
                        0x0FU
                    );

                if (
                    endpoint->endpoint_address &
                    USB_ENDPOINT_DIRECTION_IN
                )
                {
                    device->bulk_in_endpoint =
                        endpoint_number;
                    device->bulk_in_packet_size =
                        packet_size;
                }
                else
                {
                    device->bulk_out_endpoint =
                        endpoint_number;
                    device->bulk_out_packet_size =
                        packet_size;
                }
            }
        }

        offset =
            (uint16_t)(
                offset + descriptor_length
            );
    }

    if (
        device->mass_storage &&
        (
            device->bulk_in_endpoint == 0 ||
            device->bulk_out_endpoint == 0
        )
    )
    {
        device->mass_storage = false;
    }
}

static uint8_t device_effective_class(
    const usb_device_t *device
)
{
    if (device == NULL)
    {
        return 0;
    }

    return device->device_class != 0 ?
        device->device_class :
        device->interface_class;
}

static bool enumerate_device(
    uint8_t address,
    bool low_speed,
    uint8_t port,
    uint8_t parent_address,
    uint8_t depth,
    usb_device_t *device
)
{
    uint8_t first_bytes[8];
    memory_clear(
        first_bytes,
        sizeof(first_bytes)
    );

    if (
        !get_descriptor(
            0,
            USB_DESCRIPTOR_DEVICE,
            0,
            first_bytes,
            sizeof(first_bytes),
            8,
            low_speed
        )
    )
    {
        return false;
    }

    uint8_t max_packet_size =
        first_bytes[7];

    if (
        max_packet_size != 8 &&
        max_packet_size != 16 &&
        max_packet_size != 32 &&
        max_packet_size != 64
    )
    {
        max_packet_size = 8;
    }

    if (
        !set_address(
            address,
            max_packet_size,
            low_speed
        )
    )
    {
        return false;
    }

    short_delay();

    usb_device_descriptor_t descriptor;
    memory_clear(
        &descriptor,
        sizeof(descriptor)
    );

    if (
        !get_descriptor(
            address,
            USB_DESCRIPTOR_DEVICE,
            0,
            &descriptor,
            sizeof(descriptor),
            max_packet_size,
            low_speed
        )
    )
    {
        return false;
    }

    uint8_t configuration_header[9];
    memory_clear(
        configuration_header,
        sizeof(configuration_header)
    );

    if (
        !get_descriptor(
            address,
            USB_DESCRIPTOR_CONFIGURATION,
            0,
            configuration_header,
            sizeof(configuration_header),
            max_packet_size,
            low_speed
        )
    )
    {
        return false;
    }

    const usb_configuration_descriptor_t *configuration =
        (const usb_configuration_descriptor_t *)
            configuration_header;

    uint16_t total_length =
        configuration->total_length;

    uint8_t configuration_value =
        configuration->configuration_value;

    if (
        total_length <
            sizeof(usb_configuration_descriptor_t) ||
        total_length >
            USB_MAX_CONFIGURATION_BYTES
    )
    {
        return false;
    }

    uint8_t configuration_bytes[
        USB_MAX_CONFIGURATION_BYTES
    ];

    memory_clear(
        configuration_bytes,
        sizeof(configuration_bytes)
    );

    if (
        !get_descriptor(
            address,
            USB_DESCRIPTOR_CONFIGURATION,
            0,
            configuration_bytes,
            total_length,
            max_packet_size,
            low_speed
        )
    )
    {
        return false;
    }

    memory_clear(
        device,
        sizeof(*device)
    );

    device->present = true;
    device->port = port;
    device->parent_address = parent_address;
    device->depth = depth;
    device->address = address;
    device->low_speed = low_speed;
    device->vendor_id = descriptor.vendor_id;
    device->product_id = descriptor.product_id;
    device->usb_version = descriptor.usb_version;
    device->device_version =
        descriptor.device_version;
    device->device_class =
        descriptor.device_class;
    device->device_subclass =
        descriptor.device_subclass;
    device->device_protocol =
        descriptor.device_protocol;
    device->max_packet_size =
        descriptor.max_packet_size;
    device->configuration_count =
        descriptor.configuration_count;

    parse_configuration(
        device,
        configuration_bytes,
        total_length
    );

    if (
        configuration_value != 0 &&
        !set_configuration(
            address,
            configuration_value,
            max_packet_size,
            low_speed
        )
    )
    {
        return false;
    }

    if (
        device->hid_boot_keyboard ||
        device->hid_boot_mouse
    )
    {
        (void)set_hid_protocol(device);
        (void)set_hid_idle(device);
    }

    device->is_hub =
        device_effective_class(device) ==
            USB_CLASS_HUB;

    return true;
}

static bool enumerate_root_port(
    uint8_t port,
    uint8_t address,
    usb_device_t *device
)
{
    bool low_speed = false;

    if (!reset_port(port, &low_speed))
    {
        return false;
    }

    return enumerate_device(
        address,
        low_speed,
        port,
        0,
        0,
        device
    );
}

static bool hub_get_descriptor(
    const usb_device_t *hub,
    usb_hub_descriptor_t *descriptor
)
{
    if (
        hub == NULL ||
        descriptor == NULL
    )
    {
        return false;
    }

    memory_clear(
        descriptor,
        sizeof(*descriptor)
    );

    return control_transfer(
        hub->address,
        USB_REQUEST_TYPE_HUB_IN,
        USB_REQUEST_GET_DESCRIPTOR,
        (uint16_t)(
            USB_DESCRIPTOR_HUB << 8
        ),
        0,
        descriptor,
        sizeof(*descriptor),
        hub->max_packet_size,
        hub->low_speed
    );
}

static bool hub_get_port_status(
    const usb_device_t *hub,
    uint8_t port,
    usb_hub_port_status_t *status
)
{
    if (
        hub == NULL ||
        status == NULL ||
        port == 0
    )
    {
        return false;
    }

    memory_clear(
        status,
        sizeof(*status)
    );

    return control_transfer(
        hub->address,
        USB_REQUEST_TYPE_HUB_PORT_IN,
        USB_REQUEST_GET_STATUS,
        0,
        port,
        status,
        sizeof(*status),
        hub->max_packet_size,
        hub->low_speed
    );
}

static bool hub_set_port_feature(
    const usb_device_t *hub,
    uint8_t port,
    uint16_t feature
)
{
    if (
        hub == NULL ||
        port == 0
    )
    {
        return false;
    }

    return control_transfer(
        hub->address,
        USB_REQUEST_TYPE_HUB_PORT_OUT,
        USB_REQUEST_SET_FEATURE,
        feature,
        port,
        NULL,
        0,
        hub->max_packet_size,
        hub->low_speed
    );
}

static void hub_clear_port_feature(
    const usb_device_t *hub,
    uint8_t port,
    uint16_t feature
)
{
    if (
        hub == NULL ||
        port == 0
    )
    {
        return;
    }

    (void)control_transfer(
        hub->address,
        USB_REQUEST_TYPE_HUB_PORT_OUT,
        USB_REQUEST_CLEAR_FEATURE,
        feature,
        port,
        NULL,
        0,
        hub->max_packet_size,
        hub->low_speed
    );
}

static bool hub_reset_port(
    const usb_device_t *hub,
    uint8_t port,
    bool *low_speed
)
{
    usb_hub_port_status_t status;

    if (
        !hub_get_port_status(
            hub,
            port,
            &status
        ) ||
        !(status.status &
            USB_HUB_PORT_CONNECTION)
    )
    {
        return false;
    }

    if (
        !hub_set_port_feature(
            hub,
            port,
            USB_HUB_FEATURE_PORT_RESET
        )
    )
    {
        return false;
    }

    for (
        uint32_t attempt = 0;
        attempt < 100;
        attempt++
    )
    {
        short_delay();

        if (
            !hub_get_port_status(
                hub,
                port,
                &status
            )
        )
        {
            continue;
        }

        if (
            (status.status &
                USB_HUB_PORT_CONNECTION) &&
            (status.status &
                USB_HUB_PORT_ENABLE) &&
            !(status.status &
                USB_HUB_PORT_RESET)
        )
        {
            if (low_speed != NULL)
            {
                *low_speed =
                    (status.status &
                        USB_HUB_PORT_LOW_SPEED) != 0;
            }

            if (
                status.change &
                USB_HUB_CHANGE_CONNECTION
            )
            {
                hub_clear_port_feature(
                    hub,
                    port,
                    USB_HUB_FEATURE_C_CONNECTION
                );
            }

            if (
                status.change &
                USB_HUB_CHANGE_ENABLE
            )
            {
                hub_clear_port_feature(
                    hub,
                    port,
                    USB_HUB_FEATURE_C_ENABLE
                );
            }

            if (
                status.change &
                USB_HUB_CHANGE_RESET
            )
            {
                hub_clear_port_feature(
                    hub,
                    port,
                    USB_HUB_FEATURE_C_RESET
                );
            }

            return true;
        }
    }

    return false;
}

static void enumerate_hub_children(
    uint8_t hub_index
)
{
    if (
        hub_index >= enumerated_device_count
    )
    {
        return;
    }

    usb_device_t *hub =
        &devices[hub_index];

    if (
        !hub->is_hub ||
        hub->depth >= USB_MAX_HUB_DEPTH
    )
    {
        return;
    }

    usb_hub_descriptor_t descriptor;

    if (
        !hub_get_descriptor(
            hub,
            &descriptor
        ) ||
        descriptor.port_count == 0
    )
    {
        return;
    }

    hub->hub_port_count =
        descriptor.port_count;

    for (
        uint8_t port = 1;
        port <= descriptor.port_count;
        port++
    )
    {
        if (
            enumerated_device_count >=
                USB_MAX_DEVICES
        )
        {
            return;
        }

        if (
            !hub_set_port_feature(
                hub,
                port,
                USB_HUB_FEATURE_PORT_POWER
            )
        )
        {
            continue;
        }

        uint32_t power_delays =
            descriptor.power_on_to_power_good;

        if (power_delays == 0)
        {
            power_delays = 1;
        }

        for (
            uint32_t delay = 0;
            delay <= power_delays;
            delay++
        )
        {
            short_delay();
        }

        usb_hub_port_status_t status;

        if (
            !hub_get_port_status(
                hub,
                port,
                &status
            ) ||
            !(status.status &
                USB_HUB_PORT_CONNECTION)
        )
        {
            continue;
        }

        bool low_speed = false;

        if (
            !hub_reset_port(
                hub,
                port,
                &low_speed
            )
        )
        {
            continue;
        }

        uint8_t address =
            (uint8_t)(
                enumerated_device_count + 1
            );

        if (
            enumerate_device(
                address,
                low_speed,
                port,
                hub->address,
                (uint8_t)(hub->depth + 1),
                &devices[
                    enumerated_device_count
                ]
            )
        )
        {
            enumerated_device_count++;
        }
    }
}

static void initialize_interrupt_pipe(
    usb_interrupt_pipe_t *pipe,
    uint8_t slot,
    uint8_t device_index
)
{
    usb_device_t *device =
        &devices[device_index];

    uint64_t queue_offset =
        (uint64_t)slot * 0x20ULL;

    uint64_t descriptor_offset =
        0x100ULL +
        (uint64_t)slot * 0x20ULL;

    uint64_t buffer_offset =
        0x200ULL +
        (uint64_t)slot *
            USB_MAX_INTERRUPT_PACKET;

    pipe->active = true;
    pipe->device_index = device_index;
    pipe->data_toggle = false;

    uint16_t packet_size =
        device->interrupt_packet_size;

    if (
        packet_size == 0 ||
        packet_size >
            USB_MAX_INTERRUPT_PACKET
    )
    {
        packet_size =
            USB_MAX_INTERRUPT_PACKET;
    }

    pipe->packet_length =
        (uint8_t)packet_size;

    pipe->queue_head =
        (uhci_queue_head_t *)(
            interrupt_area +
            queue_offset
        );

    pipe->queue_head_physical =
        interrupt_area_physical +
        queue_offset;

    pipe->descriptor =
        (uhci_transfer_descriptor_t *)(
            interrupt_area +
            descriptor_offset
        );

    pipe->descriptor_physical =
        interrupt_area_physical +
        descriptor_offset;

    pipe->report_buffer =
        interrupt_area +
        buffer_offset;

    pipe->report_buffer_physical =
        interrupt_area_physical +
        buffer_offset;

    memory_clear(
        pipe->report_buffer,
        USB_MAX_INTERRUPT_PACKET
    );

    pipe->queue_head->head_link =
        UHCI_LINK_TERMINATE;

    pipe->queue_head->element_link =
        (uint32_t)pipe->descriptor_physical;

    pipe->descriptor->link =
        UHCI_LINK_TERMINATE;

    pipe->descriptor->control_status =
        UHCI_TD_ACTIVE |
        UHCI_TD_ERROR_COUNT_3 |
        (device->low_speed ?
            UHCI_TD_LOW_SPEED : 0U) |
        UHCI_TD_SHORT_PACKET;

    pipe->descriptor->token =
        td_token(
            UHCI_PID_IN,
            device->address,
            device->interrupt_endpoint,
            false,
            pipe->packet_length
        );

    pipe->descriptor->buffer =
        (uint32_t)pipe->report_buffer_physical;
}

static void configure_interrupt_schedule(void)
{
    keyboard_ready = false;
    mouse_ready = false;

    memory_clear(
        interrupt_pipes,
        sizeof(interrupt_pipes)
    );

    uint8_t pipe_count = 0;

    for (
        uint8_t index = 0;
        index < enumerated_device_count;
        index++
    )
    {
        usb_device_t *device =
            &devices[index];

        if (
            (!device->hid_boot_keyboard &&
             !device->hid_boot_mouse) ||
            device->interrupt_endpoint == 0 ||
            pipe_count >= USB_MAX_DEVICES
        )
        {
            continue;
        }

        initialize_interrupt_pipe(
            &interrupt_pipes[pipe_count],
            pipe_count,
            index
        );

        if (device->hid_boot_keyboard)
        {
            keyboard_ready = true;
        }

        if (device->hid_boot_mouse)
        {
            mouse_ready = true;
        }

        pipe_count++;
    }

    for (
        uint8_t index = 0;
        index < pipe_count;
        index++
    )
    {
        usb_interrupt_pipe_t *pipe =
            &interrupt_pipes[index];

        if (index + 1 < pipe_count)
        {
            pipe->queue_head->head_link =
                (uint32_t)(
                    interrupt_pipes[index + 1]
                        .queue_head_physical |
                    UHCI_LINK_QH
                );
        }
        else
        {
            pipe->queue_head->head_link =
                UHCI_LINK_TERMINATE;
        }
    }

    control_qh->head_link =
        pipe_count == 0 ?
            UHCI_LINK_TERMINATE :
            (uint32_t)(
                interrupt_pipes[0]
                    .queue_head_physical |
                UHCI_LINK_QH
            );

    keyboard_set_usb_active(
        keyboard_ready
    );

    mouse_set_usb_active(
        mouse_ready
    );

    __asm__ volatile(
        "mfence"
        :
        :
        : "memory"
    );
}

static uint16_t completed_length(
    uint32_t control_status
)
{
    uint16_t encoded =
        (uint16_t)(
            control_status &
            0x07FFU
        );

    if (encoded == 0x07FFU)
    {
        return 0;
    }

    return (uint16_t)(encoded + 1);
}

static void rearm_interrupt_pipe(
    usb_interrupt_pipe_t *pipe,
    bool advance_toggle
)
{
    usb_device_t *device =
        &devices[pipe->device_index];

    if (advance_toggle)
    {
        pipe->data_toggle =
            !pipe->data_toggle;
    }

    memory_clear(
        pipe->report_buffer,
        USB_MAX_INTERRUPT_PACKET
    );

    pipe->descriptor->link =
        UHCI_LINK_TERMINATE;

    pipe->descriptor->token =
        td_token(
            UHCI_PID_IN,
            device->address,
            device->interrupt_endpoint,
            pipe->data_toggle,
            pipe->packet_length
        );

    pipe->descriptor->buffer =
        (uint32_t)pipe->report_buffer_physical;

    pipe->descriptor->control_status =
        UHCI_TD_ACTIVE |
        UHCI_TD_ERROR_COUNT_3 |
        (device->low_speed ?
            UHCI_TD_LOW_SPEED : 0U) |
        UHCI_TD_SHORT_PACKET;

    __asm__ volatile(
        "mfence"
        :
        :
        : "memory"
    );

    pipe->queue_head->element_link =
        (uint32_t)pipe->descriptor_physical;

    __asm__ volatile(
        "mfence"
        :
        :
        : "memory"
    );
}

bool usb_init(void)
{
    controller = NULL;
    io_base = 0;
    frame_list = NULL;
    frame_list_physical = 0;
    control_qh = NULL;
    control_qh_physical = 0;
    transfer_descriptors = NULL;
    transfer_descriptors_physical = 0;
    setup_packet = NULL;
    setup_packet_physical = 0;
    transfer_buffer = NULL;
    transfer_buffer_physical = 0;
    interrupt_area = NULL;
    interrupt_area_physical = 0;
    ready = false;
    enumerated_device_count = 0;
    keyboard_ready = false;
    mouse_ready = false;

    memory_clear(
        devices,
        sizeof(devices)
    );

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

    if (!create_dma_area())
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

    if (!ready)
    {
        return false;
    }

    for (
        uint8_t port = 1;
        port <= UHCI_ROOT_PORT_COUNT;
        port++
    )
    {
        uint16_t status =
            inw(port_register(port));

        if (
            !(status & UHCI_PORT_CONNECTED) ||
            enumerated_device_count >=
                USB_MAX_DEVICES
        )
        {
            continue;
        }

        uint8_t address =
            (uint8_t)(
                enumerated_device_count + 1
            );

        if (
            enumerate_root_port(
                port,
                address,
                &devices[
                    enumerated_device_count
                ]
            )
        )
        {
            enumerated_device_count++;
        }
    }

    /*
     * QEMU automatically inserts USB hubs when the two UHCI root ports
     * are not enough. Walk every discovered hub, including hubs found
     * below another hub, before creating HID interrupt pipes.
     */
    for (
        uint8_t index = 0;
        index < enumerated_device_count;
        index++
    )
    {
        if (devices[index].is_hub)
        {
            enumerate_hub_children(index);
        }
    }

    configure_interrupt_schedule();

    return true;
}

bool usb_is_ready(void)
{
    return ready;
}


void usb_poll(void)
{
    if (!ready)
    {
        return;
    }

    for (
        uint8_t index = 0;
        index < USB_MAX_DEVICES;
        index++
    )
    {
        usb_interrupt_pipe_t *pipe =
            &interrupt_pipes[index];

        if (!pipe->active)
        {
            continue;
        }

        uint32_t status =
            pipe->descriptor->control_status;

        if (status & UHCI_TD_ACTIVE)
        {
            continue;
        }

        bool success =
            (status & UHCI_TD_ERROR_MASK) == 0;

        if (success)
        {
            uint16_t length =
                completed_length(status);

            usb_device_t *device =
                &devices[pipe->device_index];

            if (
                length >
                pipe->packet_length
            )
            {
                length =
                    pipe->packet_length;
            }

            if (
                device->hid_boot_keyboard &&
                length >= 8
            )
            {
                keyboard_handle_usb_boot_report(
                    pipe->report_buffer
                );
            }
            else if (
                device->hid_boot_mouse &&
                length >= 3
            )
            {
                mouse_handle_usb_boot_report(
                    pipe->report_buffer,
                    (uint8_t)length
                );
            }

            device->report_count++;
        }

        rearm_interrupt_pipe(
            pipe,
            success
        );
    }
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

uint8_t usb_device_count(void)
{
    return enumerated_device_count;
}

const usb_device_t *usb_device(uint8_t index)
{
    if (index >= enumerated_device_count)
    {
        return NULL;
    }

    return &devices[index];
}

bool usb_keyboard_ready(void)
{
    return keyboard_ready;
}

bool usb_mouse_ready(void)
{
    return mouse_ready;
}

bool usb_control_request_device(
    const usb_device_t *device,
    uint8_t request_type,
    uint8_t request,
    uint16_t value,
    uint16_t index,
    void *data,
    uint16_t length
)
{
    if (device == NULL)
    {
        return false;
    }

    return control_transfer(
        device->address,
        request_type,
        request,
        value,
        index,
        data,
        length,
        device->max_packet_size,
        device->low_speed
    );
}

bool usb_bulk_transfer_device(
    const usb_device_t *device,
    uint8_t endpoint,
    bool direction_in,
    uint16_t packet_size,
    bool *data_toggle,
    void *data,
    uint16_t length
)
{
    return bulk_transfer(
        device,
        endpoint,
        direction_in,
        packet_size,
        data_toggle,
        data,
        length
    );
}

static const char *class_name(uint8_t class_code)
{
    switch (class_code)
    {
        case 0x03:
            return "HID";

        case 0x08:
            return "Mass storage";

        case 0x09:
            return "Hub";

        default:
            return "Other";
    }
}

void usb_print_devices(void)
{
    if (enumerated_device_count == 0)
    {
        kprintf(
            "USB devices: none enumerated\n"
        );
        return;
    }

    kprintf(
        "USB devices: %u\n",
        (uint32_t)enumerated_device_count
    );

    for (
        uint8_t index = 0;
        index < enumerated_device_count;
        index++
    )
    {
        const usb_device_t *device =
            &devices[index];

        uint8_t effective_class =
            device->device_class != 0 ?
                device->device_class :
                device->interface_class;

        if (device->parent_address == 0)
        {
            kprintf(
                "[%u] root-port=%u address=%u speed=%s VID:PID=%04X:%04X\n",
                (uint32_t)index,
                (uint32_t)device->port,
                (uint32_t)device->address,
                device->low_speed ?
                    "low" :
                    "full",
                (uint32_t)device->vendor_id,
                (uint32_t)device->product_id
            );
        }
        else
        {
            kprintf(
                "[%u] hub=%u port=%u address=%u speed=%s VID:PID=%04X:%04X\n",
                (uint32_t)index,
                (uint32_t)device->parent_address,
                (uint32_t)device->port,
                (uint32_t)device->address,
                device->low_speed ?
                    "low" :
                    "full",
                (uint32_t)device->vendor_id,
                (uint32_t)device->product_id
            );
        }

        kprintf(
            "    USB=%04X device=%04X EP0=%u configurations=%u\n",
            (uint32_t)device->usb_version,
            (uint32_t)device->device_version,
            (uint32_t)device->max_packet_size,
            (uint32_t)device->configuration_count
        );

        kprintf(
            "    class=%02X/%02X/%02X %s interfaces=%u endpoints=%u\n",
            (uint32_t)effective_class,
            (uint32_t)device->interface_subclass,
            (uint32_t)device->interface_protocol,
            class_name(effective_class),
            (uint32_t)device->interface_count,
            (uint32_t)device->endpoint_count
        );

        if (device->is_hub)
        {
            kprintf(
                "    driver=USB hub downstream-ports=%u\n",
                (uint32_t)device->hub_port_count
            );
        }

        if (
            device->hid_boot_keyboard ||
            device->hid_boot_mouse
        )
        {
            kprintf(
                "    driver=%s endpoint=%u packet=%u interval=%u reports=%llu\n",
                device->hid_boot_keyboard ?
                    "USB HID keyboard" :
                    "USB HID mouse",
                (uint32_t)device->interrupt_endpoint,
                (uint32_t)device->interrupt_packet_size,
                (uint32_t)device->interrupt_interval,
                (unsigned long long)device->report_count
            );
        }

        if (device->mass_storage)
        {
            kprintf(
                "    driver=USB mass storage bulk-in=%u/%u bulk-out=%u/%u\n",
                (uint32_t)device->bulk_in_endpoint,
                (uint32_t)device->bulk_in_packet_size,
                (uint32_t)device->bulk_out_endpoint,
                (uint32_t)device->bulk_out_packet_size
            );
        }
    }
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
        "Controller: %s, connected ports: %u, enumerated devices: %u\n",
        ready ? "running" : "stopped",
        (uint32_t)usb_connected_port_count(),
        (uint32_t)enumerated_device_count
    );


    const char *mouse_source;

    if (mouse_usb_active())
    {
        mouse_source = "USB HID";
    }
    else if (mouse_ready)
    {
        mouse_source = "PS/2 fallback (USB pending)";
    }
    else
    {
        mouse_source = "PS/2";
    }

    kprintf(
        "Input drivers: keyboard=%s mouse=%s\n",
        keyboard_ready ? "USB HID" : "PS/2",
        mouse_source
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

    usb_print_devices();
}
