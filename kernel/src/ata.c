#include "ata.h"

#include "io.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ATA_DATA          0x1F0
#define ATA_ERROR         0x1F1
#define ATA_SECTOR_COUNT  0x1F2
#define ATA_LBA_LOW       0x1F3
#define ATA_LBA_MIDDLE    0x1F4
#define ATA_LBA_HIGH      0x1F5
#define ATA_DRIVE_SELECT  0x1F6
#define ATA_STATUS        0x1F7
#define ATA_COMMAND       0x1F7
#define ATA_CONTROL       0x3F6
#define ATA_ALT_STATUS    0x3F6

#define ATA_COMMAND_IDENTIFY    0xEC
#define ATA_COMMAND_READ_PIO    0x20
#define ATA_COMMAND_WRITE_PIO   0x30
#define ATA_COMMAND_CACHE_FLUSH 0xE7

#define ATA_STATUS_ERROR        0x01
#define ATA_STATUS_DATA_REQUEST 0x08
#define ATA_STATUS_DEVICE_FAULT 0x20
#define ATA_STATUS_BUSY         0x80

#define ATA_IDENTIFY_WORDS      256
#define ATA_SECTOR_SIZE         512
#define ATA_TIMEOUT             1000000U
#define ATA_LBA28_LIMIT         0x0FFFFFFFU

static bool initialized;
static uint32_t total_sectors;
static char model[41];
static block_device_t device;

static void ata_delay_400ns(void)
{
    (void)inb(ATA_ALT_STATUS);
    (void)inb(ATA_ALT_STATUS);
    (void)inb(ATA_ALT_STATUS);
    (void)inb(ATA_ALT_STATUS);
}

static bool ata_wait_not_busy(void)
{
    for (
        uint32_t timeout = 0;
        timeout < ATA_TIMEOUT;
        timeout++
    )
    {
        uint8_t status = inb(ATA_STATUS);

        if (
            status &
            (ATA_STATUS_ERROR |
             ATA_STATUS_DEVICE_FAULT)
        )
        {
            return false;
        }

        if (!(status & ATA_STATUS_BUSY))
        {
            return true;
        }

        __asm__ volatile("pause");
    }

    return false;
}

static bool ata_wait_data_request(void)
{
    for (
        uint32_t timeout = 0;
        timeout < ATA_TIMEOUT;
        timeout++
    )
    {
        uint8_t status = inb(ATA_STATUS);

        if (
            status &
            (ATA_STATUS_ERROR |
             ATA_STATUS_DEVICE_FAULT)
        )
        {
            return false;
        }

        if (
            !(status & ATA_STATUS_BUSY) &&
            (status & ATA_STATUS_DATA_REQUEST)
        )
        {
            return true;
        }

        __asm__ volatile("pause");
    }

    return false;
}

static void ata_select_lba28(uint32_t lba)
{
    outb(
        ATA_DRIVE_SELECT,
        (uint8_t)(
            0xE0U |
            ((lba >> 24) & 0x0FU)
        )
    );

    ata_delay_400ns();
}

static bool ata_read_callback(
    void *context,
    uint64_t lba,
    uint32_t sector_count,
    void *buffer
)
{
    (void)context;

    if (
        !initialized ||
        buffer == NULL ||
        sector_count == 0 ||
        sector_count > 255 ||
        lba > ATA_LBA28_LIMIT ||
        lba >= total_sectors ||
        sector_count > total_sectors - lba
    )
    {
        return false;
    }

    uint8_t *output = buffer;
    uint32_t current_lba = (uint32_t)lba;

    ata_select_lba28(current_lba);

    outb(
        ATA_SECTOR_COUNT,
        (uint8_t)sector_count
    );

    outb(
        ATA_LBA_LOW,
        (uint8_t)current_lba
    );

    outb(
        ATA_LBA_MIDDLE,
        (uint8_t)(current_lba >> 8)
    );

    outb(
        ATA_LBA_HIGH,
        (uint8_t)(current_lba >> 16)
    );

    outb(
        ATA_COMMAND,
        ATA_COMMAND_READ_PIO
    );

    for (
        uint32_t sector = 0;
        sector < sector_count;
        sector++
    )
    {
        if (!ata_wait_data_request())
        {
            return false;
        }

        for (
            uint32_t word = 0;
            word < ATA_SECTOR_SIZE / 2;
            word++
        )
        {
            uint16_t value = inw(ATA_DATA);

            output[0] =
                (uint8_t)(value & 0xFF);

            output[1] =
                (uint8_t)(value >> 8);

            output += 2;
        }

        ata_delay_400ns();
    }

    return true;
}

static bool ata_write_callback(
    void *context,
    uint64_t lba,
    uint32_t sector_count,
    const void *buffer
)
{
    (void)context;

    if (
        !initialized ||
        buffer == NULL ||
        sector_count == 0 ||
        sector_count > 255 ||
        lba > ATA_LBA28_LIMIT ||
        lba >= total_sectors ||
        sector_count > total_sectors - lba
    )
    {
        return false;
    }

    const uint8_t *input = buffer;
    uint32_t current_lba = (uint32_t)lba;

    ata_select_lba28(current_lba);

    outb(
        ATA_SECTOR_COUNT,
        (uint8_t)sector_count
    );

    outb(
        ATA_LBA_LOW,
        (uint8_t)current_lba
    );

    outb(
        ATA_LBA_MIDDLE,
        (uint8_t)(current_lba >> 8)
    );

    outb(
        ATA_LBA_HIGH,
        (uint8_t)(current_lba >> 16)
    );

    outb(
        ATA_COMMAND,
        ATA_COMMAND_WRITE_PIO
    );

    for (
        uint32_t sector = 0;
        sector < sector_count;
        sector++
    )
    {
        if (!ata_wait_data_request())
        {
            return false;
        }

        for (
            uint32_t word = 0;
            word < ATA_SECTOR_SIZE / 2;
            word++
        )
        {
            uint16_t value =
                (uint16_t)input[0] |
                ((uint16_t)input[1] << 8);

            outw(
                ATA_DATA,
                value
            );

            input += 2;
        }

        ata_delay_400ns();
    }

    outb(
        ATA_COMMAND,
        ATA_COMMAND_CACHE_FLUSH
    );

    return ata_wait_not_busy();
}

static void ata_parse_model(
    const uint16_t identify[ATA_IDENTIFY_WORDS]
)
{
    uint32_t output = 0;

    for (
        uint32_t word = 27;
        word <= 46;
        word++
    )
    {
        model[output] =
            (char)(identify[word] >> 8);

        model[output + 1] =
            (char)(identify[word] & 0xFF);

        output += 2;
    }

    while (
        output > 0 &&
        model[output - 1] == ' '
    )
    {
        output--;
    }

    model[output] = '\0';

    if (output == 0)
    {
        model[0] = 'A';
        model[1] = 'T';
        model[2] = 'A';
        model[3] = ' ';
        model[4] = 'd';
        model[5] = 'i';
        model[6] = 's';
        model[7] = 'k';
        model[8] = '\0';
    }
}

bool ata_init(void)
{
    initialized = false;
    total_sectors = 0;
    model[0] = '\0';

    /* Use polling rather than ATA hardware interrupts. */
    outb(ATA_CONTROL, 0x02);

    ata_select_lba28(0);

    outb(ATA_SECTOR_COUNT, 0);
    outb(ATA_LBA_LOW, 0);
    outb(ATA_LBA_MIDDLE, 0);
    outb(ATA_LBA_HIGH, 0);
    outb(ATA_COMMAND, ATA_COMMAND_IDENTIFY);

    uint8_t status = inb(ATA_STATUS);

    if (status == 0)
    {
        return false;
    }

    if (!ata_wait_not_busy())
    {
        return false;
    }

    if (
        inb(ATA_LBA_MIDDLE) != 0 ||
        inb(ATA_LBA_HIGH) != 0
    )
    {
        return false;
    }

    if (!ata_wait_data_request())
    {
        return false;
    }

    uint16_t identify[ATA_IDENTIFY_WORDS];

    for (
        uint32_t word = 0;
        word < ATA_IDENTIFY_WORDS;
        word++
    )
    {
        identify[word] = inw(ATA_DATA);
    }

    if (!(identify[49] & (1U << 9)))
    {
        return false;
    }

    total_sectors =
        (uint32_t)identify[60] |
        ((uint32_t)identify[61] << 16);

    if (total_sectors == 0)
    {
        return false;
    }

    ata_parse_model(identify);

    device.name = model;
    device.sector_size = ATA_SECTOR_SIZE;
    device.sector_count = total_sectors;
    device.writable = true;
    device.context = NULL;
    device.read = ata_read_callback;
    device.write = ata_write_callback;

    initialized = true;
    return true;
}

bool ata_available(void)
{
    return initialized;
}

const block_device_t *ata_block_device(void)
{
    if (!initialized)
    {
        return NULL;
    }

    return &device;
}
