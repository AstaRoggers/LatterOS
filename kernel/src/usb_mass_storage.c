#include "usb_mass_storage.h"

#include "block_device.h"
#include "kstdio.h"
#include "usb.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define USB_REQUEST_CLEAR_FEATURE        0x01
#define USB_MASS_REQUEST_RESET           0xFF
#define USB_MASS_REQUEST_GET_MAX_LUN     0xFE

#define USB_REQUEST_TYPE_CLASS_IN_IFACE  0xA1
#define USB_REQUEST_TYPE_CLASS_OUT_IFACE 0x21
#define USB_REQUEST_TYPE_OUT_ENDPOINT    0x02
#define USB_FEATURE_ENDPOINT_HALT        0x0000

#define USB_ENDPOINT_DIRECTION_IN        0x80

#define USB_MASS_CBW_SIGNATURE 0x43425355U
#define USB_MASS_CSW_SIGNATURE 0x53425355U
#define USB_MASS_CBW_FLAG_IN   0x80

#define USB_MASS_CSW_PASSED       0x00
#define USB_MASS_CSW_FAILED       0x01
#define USB_MASS_CSW_PHASE_ERROR  0x02

#define SCSI_TEST_UNIT_READY  0x00
#define SCSI_REQUEST_SENSE    0x03
#define SCSI_INQUIRY          0x12
#define SCSI_READ_CAPACITY_10 0x25
#define SCSI_READ_10          0x28
#define SCSI_WRITE_10         0x2A
#define SCSI_SYNCHRONIZE_CACHE_10 0x35

#define SCSI_INQUIRY_LENGTH 36
#define SCSI_SENSE_LENGTH   18
#define SCSI_CAPACITY_LENGTH 8

#define USB_MASS_MAX_BLOCK_SIZE 3072U
#define USB_MASS_READY_RETRIES  8U

typedef struct __attribute__((packed))
{
    uint32_t signature;
    uint32_t tag;
    uint32_t transfer_length;
    uint8_t flags;
    uint8_t lun;
    uint8_t command_length;
    uint8_t command[16];
} usb_mass_cbw_t;

typedef struct __attribute__((packed))
{
    uint32_t signature;
    uint32_t tag;
    uint32_t residue;
    uint8_t status;
} usb_mass_csw_t;

typedef struct
{
    bool present;
    uint8_t usb_device_index;
    uint8_t lun;
    bool bulk_in_toggle;
    bool bulk_out_toggle;
    uint32_t next_tag;

    uint32_t block_size;
    uint64_t block_count;

    char vendor[9];
    char product[17];
    char revision[5];
    char block_name[32];

    block_device_t block_device;
} usb_mass_device_t;

static usb_mass_device_t mass_devices[
    USB_MASS_STORAGE_MAX_DEVICES
];

static uint8_t mass_device_count;
static bool initialized;

static void clear_bytes(
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

static void copy_bytes(
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

static uint32_t read_be32(
    const uint8_t *bytes
)
{
    return
        ((uint32_t)bytes[0] << 24) |
        ((uint32_t)bytes[1] << 16) |
        ((uint32_t)bytes[2] << 8) |
        (uint32_t)bytes[3];
}

static void write_be16(
    uint8_t *bytes,
    uint16_t value
)
{
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}

static void write_be32(
    uint8_t *bytes,
    uint32_t value
)
{
    bytes[0] = (uint8_t)(value >> 24);
    bytes[1] = (uint8_t)(value >> 16);
    bytes[2] = (uint8_t)(value >> 8);
    bytes[3] = (uint8_t)value;
}

static void copy_scsi_text(
    char *destination,
    size_t capacity,
    const uint8_t *source,
    size_t source_length
)
{
    if (
        destination == NULL ||
        capacity == 0
    )
    {
        return;
    }

    size_t length = source_length;

    while (
        length > 0 &&
        source[length - 1] == ' '
    )
    {
        length--;
    }

    if (length >= capacity)
    {
        length = capacity - 1;
    }

    for (
        size_t index = 0;
        index < length;
        index++
    )
    {
        uint8_t character = source[index];

        destination[index] =
            character >= 32 && character <= 126 ?
                (char)character :
                '?';
    }

    destination[length] = '\0';
}

static void build_block_name(
    usb_mass_device_t *mass,
    uint8_t index
)
{
    static const char prefix[] =
        "USB mass storage disk";

    size_t position = 0;

    while (
        prefix[position] != '\0' &&
        position + 1 <
            sizeof(mass->block_name)
    )
    {
        mass->block_name[position] =
            prefix[position];
        position++;
    }

    if (
        position + 1 <
        sizeof(mass->block_name)
    )
    {
        mass->block_name[position] =
            (char)('0' + index);
        position++;
    }

    mass->block_name[position] = '\0';
}

static const usb_device_t *mass_usb_device(
    const usb_mass_device_t *mass
)
{
    if (mass == NULL)
    {
        return NULL;
    }

    return usb_device(
        mass->usb_device_index
    );
}

static void reset_transport(
    usb_mass_device_t *mass
)
{
    const usb_device_t *device =
        mass_usb_device(mass);

    if (device == NULL)
    {
        return;
    }

    (void)usb_control_request_device(
        device,
        USB_REQUEST_TYPE_CLASS_OUT_IFACE,
        USB_MASS_REQUEST_RESET,
        0,
        device->interface_number,
        NULL,
        0
    );

    (void)usb_control_request_device(
        device,
        USB_REQUEST_TYPE_OUT_ENDPOINT,
        USB_REQUEST_CLEAR_FEATURE,
        USB_FEATURE_ENDPOINT_HALT,
        (uint16_t)(
            USB_ENDPOINT_DIRECTION_IN |
            device->bulk_in_endpoint
        ),
        NULL,
        0
    );

    (void)usb_control_request_device(
        device,
        USB_REQUEST_TYPE_OUT_ENDPOINT,
        USB_REQUEST_CLEAR_FEATURE,
        USB_FEATURE_ENDPOINT_HALT,
        device->bulk_out_endpoint,
        NULL,
        0
    );

    mass->bulk_in_toggle = false;
    mass->bulk_out_toggle = false;
}

static bool bulk_command(
    usb_mass_device_t *mass,
    const uint8_t *command,
    uint8_t command_length,
    void *data,
    uint32_t data_length,
    bool data_in
)
{
    const usb_device_t *device =
        mass_usb_device(mass);

    if (
        device == NULL ||
        command == NULL ||
        command_length == 0 ||
        command_length > 16 ||
        data_length > USB_MASS_MAX_BLOCK_SIZE ||
        (data_length > 0 && data == NULL)
    )
    {
        return false;
    }

    usb_mass_cbw_t cbw;
    usb_mass_csw_t csw;

    clear_bytes(&cbw, sizeof(cbw));
    clear_bytes(&csw, sizeof(csw));

    uint32_t tag = mass->next_tag++;

    if (tag == 0)
    {
        tag = mass->next_tag++;
    }

    cbw.signature = USB_MASS_CBW_SIGNATURE;
    cbw.tag = tag;
    cbw.transfer_length = data_length;
    cbw.flags =
        data_in ?
            USB_MASS_CBW_FLAG_IN :
            0;
    cbw.lun = mass->lun;
    cbw.command_length = command_length;

    copy_bytes(
        cbw.command,
        command,
        command_length
    );

    if (
        !usb_bulk_transfer_device(
            device,
            device->bulk_out_endpoint,
            false,
            device->bulk_out_packet_size,
            &mass->bulk_out_toggle,
            &cbw,
            sizeof(cbw)
        )
    )
    {
        reset_transport(mass);
        return false;
    }

    if (
        data_length > 0 &&
        !usb_bulk_transfer_device(
            device,
            data_in ?
                device->bulk_in_endpoint :
                device->bulk_out_endpoint,
            data_in,
            data_in ?
                device->bulk_in_packet_size :
                device->bulk_out_packet_size,
            data_in ?
                &mass->bulk_in_toggle :
                &mass->bulk_out_toggle,
            data,
            (uint16_t)data_length
        )
    )
    {
        reset_transport(mass);
        return false;
    }

    if (
        !usb_bulk_transfer_device(
            device,
            device->bulk_in_endpoint,
            true,
            device->bulk_in_packet_size,
            &mass->bulk_in_toggle,
            &csw,
            sizeof(csw)
        )
    )
    {
        reset_transport(mass);
        return false;
    }

    if (
        csw.signature != USB_MASS_CSW_SIGNATURE ||
        csw.tag != tag ||
        csw.status == USB_MASS_CSW_PHASE_ERROR
    )
    {
        reset_transport(mass);
        return false;
    }

    return csw.status == USB_MASS_CSW_PASSED;
}

static bool request_sense(
    usb_mass_device_t *mass
)
{
    uint8_t command[6];
    uint8_t sense[SCSI_SENSE_LENGTH];

    clear_bytes(command, sizeof(command));
    clear_bytes(sense, sizeof(sense));

    command[0] = SCSI_REQUEST_SENSE;
    command[4] = SCSI_SENSE_LENGTH;

    return bulk_command(
        mass,
        command,
        sizeof(command),
        sense,
        sizeof(sense),
        true
    );
}

static bool test_unit_ready(
    usb_mass_device_t *mass
)
{
    uint8_t command[6];
    clear_bytes(command, sizeof(command));
    command[0] = SCSI_TEST_UNIT_READY;

    for (
        uint32_t attempt = 0;
        attempt < USB_MASS_READY_RETRIES;
        attempt++
    )
    {
        if (
            bulk_command(
                mass,
                command,
                sizeof(command),
                NULL,
                0,
                false
            )
        )
        {
            return true;
        }

        (void)request_sense(mass);
    }

    return false;
}

static bool inquiry(
    usb_mass_device_t *mass
)
{
    uint8_t command[6];
    uint8_t response[SCSI_INQUIRY_LENGTH];

    clear_bytes(command, sizeof(command));
    clear_bytes(response, sizeof(response));

    command[0] = SCSI_INQUIRY;
    command[4] = SCSI_INQUIRY_LENGTH;

    if (
        !bulk_command(
            mass,
            command,
            sizeof(command),
            response,
            sizeof(response),
            true
        )
    )
    {
        return false;
    }

    copy_scsi_text(
        mass->vendor,
        sizeof(mass->vendor),
        &response[8],
        8
    );

    copy_scsi_text(
        mass->product,
        sizeof(mass->product),
        &response[16],
        16
    );

    copy_scsi_text(
        mass->revision,
        sizeof(mass->revision),
        &response[32],
        4
    );

    return true;
}

static bool read_capacity(
    usb_mass_device_t *mass
)
{
    uint8_t command[10];
    uint8_t response[SCSI_CAPACITY_LENGTH];

    clear_bytes(command, sizeof(command));
    clear_bytes(response, sizeof(response));

    command[0] = SCSI_READ_CAPACITY_10;

    if (
        !bulk_command(
            mass,
            command,
            sizeof(command),
            response,
            sizeof(response),
            true
        )
    )
    {
        return false;
    }

    uint32_t last_lba =
        read_be32(&response[0]);

    uint32_t block_size =
        read_be32(&response[4]);

    if (
        last_lba == UINT32_MAX ||
        block_size == 0 ||
        block_size > USB_MASS_MAX_BLOCK_SIZE
    )
    {
        return false;
    }

    mass->block_count =
        (uint64_t)last_lba + 1ULL;

    mass->block_size = block_size;

    return true;
}

static bool read_one_block(
    usb_mass_device_t *mass,
    uint32_t lba,
    void *buffer
)
{
    uint8_t command[10];
    clear_bytes(command, sizeof(command));

    command[0] = SCSI_READ_10;
    write_be32(&command[2], lba);
    write_be16(&command[7], 1);

    return bulk_command(
        mass,
        command,
        sizeof(command),
        buffer,
        mass->block_size,
        true
    );
}

static bool write_one_block(
    usb_mass_device_t *mass,
    uint32_t lba,
    const void *buffer
)
{
    uint8_t command[10];
    clear_bytes(command, sizeof(command));

    command[0] = SCSI_WRITE_10;
    write_be32(&command[2], lba);
    write_be16(&command[7], 1);

    return bulk_command(
        mass,
        command,
        sizeof(command),
        (void *)buffer,
        mass->block_size,
        false
    );
}

static bool synchronize_cache(
    usb_mass_device_t *mass
)
{
    uint8_t command[10];
    clear_bytes(command, sizeof(command));

    command[0] = SCSI_SYNCHRONIZE_CACHE_10;

    return bulk_command(
        mass,
        command,
        sizeof(command),
        NULL,
        0,
        false
    );
}

static bool block_read(
    void *context,
    uint64_t lba,
    uint32_t sector_count,
    void *buffer
)
{
    usb_mass_device_t *mass = context;

    if (
        mass == NULL ||
        !mass->present ||
        buffer == NULL ||
        sector_count == 0 ||
        lba > UINT32_MAX ||
        sector_count > UINT32_MAX - lba + 1ULL
    )
    {
        return false;
    }

    uint8_t *output = buffer;

    for (
        uint32_t sector = 0;
        sector < sector_count;
        sector++
    )
    {
        if (
            !read_one_block(
                mass,
                (uint32_t)(lba + sector),
                output +
                    (uint64_t)sector *
                    mass->block_size
            )
        )
        {
            return false;
        }
    }

    return true;
}

static bool block_write(
    void *context,
    uint64_t lba,
    uint32_t sector_count,
    const void *buffer
)
{
    usb_mass_device_t *mass = context;

    if (
        mass == NULL ||
        !mass->present ||
        buffer == NULL ||
        sector_count == 0 ||
        lba > UINT32_MAX ||
        sector_count > UINT32_MAX - lba + 1ULL
    )
    {
        return false;
    }

    const uint8_t *input = buffer;

    for (
        uint32_t sector = 0;
        sector < sector_count;
        sector++
    )
    {
        if (
            !write_one_block(
                mass,
                (uint32_t)(lba + sector),
                input +
                    (uint64_t)sector *
                    mass->block_size
            )
        )
        {
            return false;
        }
    }

    return synchronize_cache(mass);
}

static bool initialize_device(
    uint8_t usb_index,
    uint8_t mass_index
)
{
    const usb_device_t *device =
        usb_device(usb_index);

    if (
        device == NULL ||
        !device->mass_storage ||
        device->bulk_in_endpoint == 0 ||
        device->bulk_out_endpoint == 0
    )
    {
        return false;
    }

    usb_mass_device_t *mass =
        &mass_devices[mass_index];

    clear_bytes(mass, sizeof(*mass));

    mass->usb_device_index = usb_index;
    mass->next_tag = 1;

    uint8_t maximum_lun = 0;

    if (
        usb_control_request_device(
            device,
            USB_REQUEST_TYPE_CLASS_IN_IFACE,
            USB_MASS_REQUEST_GET_MAX_LUN,
            0,
            device->interface_number,
            &maximum_lun,
            sizeof(maximum_lun)
        )
    )
    {
        mass->lun =
            maximum_lun > 0 ? 0 : maximum_lun;
    }

    reset_transport(mass);

    if (
        !inquiry(mass) ||
        !test_unit_ready(mass) ||
        !read_capacity(mass)
    )
    {
        clear_bytes(mass, sizeof(*mass));
        return false;
    }

    build_block_name(mass, mass_index);

    mass->block_device.name =
        mass->block_name;
    mass->block_device.sector_size =
        mass->block_size;
    mass->block_device.sector_count =
        mass->block_count;
    mass->block_device.writable = true;
    mass->block_device.context = mass;
    mass->block_device.read = block_read;
    mass->block_device.write = block_write;

    if (
        !block_device_register(
            &mass->block_device
        )
    )
    {
        clear_bytes(mass, sizeof(*mass));
        return false;
    }

    mass->present = true;
    return true;
}

bool usb_mass_storage_init(void)
{
    clear_bytes(
        mass_devices,
        sizeof(mass_devices)
    );

    mass_device_count = 0;
    initialized = true;

    for (
        uint8_t usb_index = 0;
        usb_index < usb_device_count() &&
        mass_device_count <
            USB_MASS_STORAGE_MAX_DEVICES;
        usb_index++
    )
    {
        const usb_device_t *device =
            usb_device(usb_index);

        if (
            device == NULL ||
            !device->mass_storage
        )
        {
            continue;
        }

        if (
            initialize_device(
                usb_index,
                mass_device_count
            )
        )
        {
            mass_device_count++;
        }
    }

    return mass_device_count > 0;
}

bool usb_mass_storage_ready(void)
{
    return initialized &&
        mass_device_count > 0;
}

uint8_t usb_mass_storage_count(void)
{
    return mass_device_count;
}

void usb_mass_storage_print(void)
{
    if (!initialized)
    {
        kprintf(
            "USB mass storage: not initialized\n"
        );
        return;
    }

    kprintf(
        "USB mass-storage devices: %u\n",
        (uint32_t)mass_device_count
    );

    for (
        uint8_t index = 0;
        index < mass_device_count;
        index++
    )
    {
        const usb_mass_device_t *mass =
            &mass_devices[index];

        kprintf(
            "[%u] USB device=%u LUN=%u %s %s rev=%s\n",
            (uint32_t)index,
            (uint32_t)mass->usb_device_index,
            (uint32_t)mass->lun,
            mass->vendor[0] != '\0' ?
                mass->vendor : "Unknown",
            mass->product[0] != '\0' ?
                mass->product : "disk",
            mass->revision[0] != '\0' ?
                mass->revision : "n/a"
        );

        kprintf(
            "    blocks=%llu block-size=%u capacity=%llu MiB read-write\n",
            (unsigned long long)mass->block_count,
            (uint32_t)mass->block_size,
            (unsigned long long)(
                mass->block_count *
                mass->block_size /
                (1024ULL * 1024ULL)
            )
        );
    }
}
