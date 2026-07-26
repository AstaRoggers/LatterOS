#include "shell.h"

#include "block_device.h"
#include "pci.h"
#include "storage.h"
#include "terminal.h"
#include "timer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DISK_TEST_LBA 2048ULL
#define DISK_SECTOR_BUFFER_SIZE 512

typedef void (*command_function_t)(
    const char *arguments
);

typedef struct
{
    const char *name;
    const char *description;
    command_function_t function;
} shell_command_t;

static uint8_t disk_write_buffer[
    DISK_SECTOR_BUFFER_SIZE
];

static uint8_t disk_read_buffer[
    DISK_SECTOR_BUFFER_SIZE
];

static bool strings_equal(
    const char *first,
    const char *second
)
{
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

static bool character_is_space(
    char character
)
{
    return (
        character == ' ' ||
        character == '\t'
    );
}

static bool parse_uint64(
    const char *text,
    uint64_t *value
)
{
    if (
        text == NULL ||
        value == NULL ||
        text[0] == '\0'
    )
    {
        return false;
    }

    uint64_t result = 0;
    uint32_t index = 0;

    while (text[index] != '\0')
    {
        char character = text[index];

        if (
            character < '0' ||
            character > '9'
        )
        {
            return false;
        }

        uint64_t digit =
            (uint64_t)(character - '0');

        if (
            result >
            (UINT64_MAX - digit) / 10
        )
        {
            return false;
        }

        result = result * 10 + digit;
        index++;
    }

    *value = result;
    return true;
}

static void uint64_to_string(
    uint64_t value,
    char *buffer
)
{
    char temporary[21];
    uint32_t length = 0;

    if (value == 0)
    {
        buffer[0] = '0';
        buffer[1] = '\0';
        return;
    }

    while (value > 0)
    {
        temporary[length] =
            (char)(
                '0' +
                value % 10
            );

        length++;
        value /= 10;
    }

    for (
        uint32_t index = 0;
        index < length;
        index++
    )
    {
        buffer[index] =
            temporary[
                length -
                index -
                1
            ];
    }

    buffer[length] = '\0';
}

static void terminal_write_hex_byte(
    uint8_t value
)
{
    static const char hexadecimal[] =
        "0123456789ABCDEF";

    char text[3];

    text[0] =
        hexadecimal[(value >> 4) & 0x0F];

    text[1] =
        hexadecimal[value & 0x0F];

    text[2] = '\0';

    terminal_write(text);
}

static void command_help(
    const char *arguments
);

static void command_clear(
    const char *arguments
);

static void command_version(
    const char *arguments
);

static void command_echo(
    const char *arguments
);

static void command_ticks(
    const char *arguments
);

static void command_lspci(
    const char *arguments
);

static void command_storage(
    const char *arguments
);

static void command_disks(
    const char *arguments
);

static void command_diskread(
    const char *arguments
);

static void command_disktest(
    const char *arguments
);

static void command_panic(
    const char *arguments
);

static const shell_command_t commands[] = {
    {
        "help",
        "Show available commands",
        command_help
    },
    {
        "clear",
        "Clear the screen",
        command_clear
    },
    {
        "version",
        "Show OS version",
        command_version
    },
    {
        "echo",
        "Print text",
        command_echo
    },
    {
        "ticks",
        "Show timer tick count",
        command_ticks
    },
    {
        "lspci",
        "List detected PCI devices",
        command_lspci
    },
    {
        "storage",
        "Show storage controller",
        command_storage
    },
    {
        "disks",
        "List block devices",
        command_disks
    },
    {
        "diskread",
        "Read one sector: diskread LBA",
        command_diskread
    },
    {
        "disktest",
        "Test disk write and read",
        command_disktest
    },
    {
        "panic",
        "Test CPU exception handling",
        command_panic
    }
};

#define COMMAND_COUNT \
    (sizeof(commands) / sizeof(commands[0]))

static void command_help(
    const char *arguments
)
{
    (void)arguments;

    terminal_write_line(
        "Available commands:"
    );

    for (
        uint32_t index = 0;
        index < COMMAND_COUNT;
        index++
    )
    {
        terminal_write(commands[index].name);
        terminal_write(" - ");

        terminal_write_line(
            commands[index].description
        );
    }
}

static void command_clear(
    const char *arguments
)
{
    (void)arguments;
    terminal_clear();
}

static void command_version(
    const char *arguments
)
{
    (void)arguments;

    terminal_write_line(
        "LatterOS version 0.1"
    );
}

static void command_echo(
    const char *arguments
)
{
    terminal_write_line(arguments);
}

static void command_ticks(
    const char *arguments
)
{
    (void)arguments;

    char text[21];

    uint64_to_string(
        timer_ticks(),
        text
    );

    terminal_write("Timer ticks: ");
    terminal_write_line(text);
}

static void command_lspci(
    const char *arguments
)
{
    (void)arguments;
    pci_print_devices();
}

static void command_storage(
    const char *arguments
)
{
    (void)arguments;
    storage_print_controller();
}

static void command_disks(
    const char *arguments
)
{
    (void)arguments;
    storage_print_devices();
}

static void command_diskread(
    const char *arguments
)
{
    const block_device_t *device =
        storage_primary_device();

    if (device == NULL)
    {
        terminal_write_line(
            "No block device available"
        );
        return;
    }

    uint64_t lba;

    if (!parse_uint64(arguments, &lba))
    {
        terminal_write_line(
            "Usage: diskread LBA"
        );
        return;
    }

    if (
        !block_device_read(
            device,
            lba,
            1,
            disk_read_buffer
        )
    )
    {
        terminal_write_line(
            "Disk read failed"
        );
        return;
    }

    terminal_write("Sector ");

    char lba_text[21];
    uint64_to_string(lba, lba_text);
    terminal_write(lba_text);

    terminal_write_line(
        " first 64 bytes:"
    );

    for (
        uint32_t row = 0;
        row < 4;
        row++
    )
    {
        for (
            uint32_t column = 0;
            column < 16;
            column++
        )
        {
            uint32_t index =
                row * 16 + column;

            terminal_write_hex_byte(
                disk_read_buffer[index]
            );

            terminal_write(" ");
        }

        terminal_write("|");

        for (
            uint32_t column = 0;
            column < 16;
            column++
        )
        {
            uint8_t value =
                disk_read_buffer[
                    row * 16 + column
                ];

            char text[2];

            text[0] =
                value >= 32 && value <= 126 ?
                (char)value :
                '.';

            text[1] = '\0';
            terminal_write(text);
        }

        terminal_write_line("|");
    }
}

static void command_disktest(
    const char *arguments
)
{
    (void)arguments;

    const block_device_t *device =
        storage_primary_device();

    if (device == NULL)
    {
        terminal_write_line(
            "No block device available"
        );
        return;
    }

    if (
        !device->writable ||
        device->sector_size !=
            DISK_SECTOR_BUFFER_SIZE ||
        device->sector_count <=
            DISK_TEST_LBA
    )
    {
        terminal_write_line(
            "Disk cannot run write test"
        );
        return;
    }

    for (
        uint32_t index = 0;
        index < DISK_SECTOR_BUFFER_SIZE;
        index++
    )
    {
        disk_write_buffer[index] =
            (uint8_t)(index ^ 0xA5U);

        disk_read_buffer[index] = 0;
    }

    static const char signature[] =
        "LatterOS block device test";

    for (
        uint32_t index = 0;
        signature[index] != '\0';
        index++
    )
    {
        disk_write_buffer[index] =
            (uint8_t)signature[index];
    }

    if (
        !block_device_write(
            device,
            DISK_TEST_LBA,
            1,
            disk_write_buffer
        )
    )
    {
        terminal_write_line(
            "Disk write failed"
        );
        return;
    }

    if (
        !block_device_read(
            device,
            DISK_TEST_LBA,
            1,
            disk_read_buffer
        )
    )
    {
        terminal_write_line(
            "Disk read-back failed"
        );
        return;
    }

    for (
        uint32_t index = 0;
        index < DISK_SECTOR_BUFFER_SIZE;
        index++
    )
    {
        if (
            disk_write_buffer[index] !=
            disk_read_buffer[index]
        )
        {
            terminal_write_line(
                "Disk verification failed"
            );
            return;
        }
    }

    terminal_write_line(
        "Disk write/read test passed"
    );
}

static void command_panic(
    const char *arguments
)
{
    (void)arguments;
    __asm__ volatile("int3");
}

void shell_execute(
    const char *input
)
{
    char command_name[32];

    uint32_t input_index = 0;
    uint32_t command_index = 0;

    while (
        character_is_space(
            input[input_index]
        )
    )
    {
        input_index++;
    }

    while (
        input[input_index] != '\0' &&
        !character_is_space(
            input[input_index]
        ) &&
        command_index <
            sizeof(command_name) - 1
    )
    {
        command_name[command_index] =
            input[input_index];

        command_index++;
        input_index++;
    }

    command_name[command_index] = '\0';

    if (command_index == 0)
    {
        return;
    }

    while (
        character_is_space(
            input[input_index]
        )
    )
    {
        input_index++;
    }

    const char *arguments =
        &input[input_index];

    for (
        uint32_t index = 0;
        index < COMMAND_COUNT;
        index++
    )
    {
        if (
            strings_equal(
                command_name,
                commands[index].name
            )
        )
        {
            commands[index].function(
                arguments
            );

            return;
        }
    }

    terminal_write(
        "Unknown command: "
    );

    terminal_write_line(
        command_name
    );
}
