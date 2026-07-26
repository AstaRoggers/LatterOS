#include "shell.h"

#include "block_device.h"
#include "kstdio.h"
#include "kstdlib.h"
#include "kstring.h"
#include "pci.h"
#include "process.h"
#include "storage.h"
#include "terminal.h"
#include "timer.h"
#include "vfs.h"

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

static bool split_first_argument(
    const char *arguments,
    char *first,
    size_t first_capacity,
    const char **remaining
)
{
    if (
        arguments == NULL ||
        first == NULL ||
        first_capacity < 2 ||
        remaining == NULL
    )
    {
        return false;
    }

    size_t input = 0;

    while (
        character_is_space(
            arguments[input]
        )
    )
    {
        input++;
    }

    size_t output = 0;

    while (
        arguments[input] != '\0' &&
        !character_is_space(
            arguments[input]
        )
    )
    {
        if (output + 1 >= first_capacity)
        {
            return false;
        }

        first[output] = arguments[input];
        output++;
        input++;
    }

    first[output] = '\0';

    while (
        character_is_space(
            arguments[input]
        )
    )
    {
        input++;
    }

    *remaining = &arguments[input];

    return output > 0;
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

static void command_ls(
    const char *arguments
);

static void command_cd(
    const char *arguments
);

static void command_pwd(
    const char *arguments
);

static void command_cat(
    const char *arguments
);

static void command_mkdir(
    const char *arguments
);

static void command_write(
    const char *arguments
);

static void command_ps(
    const char *arguments
);

static void command_spawn(
    const char *arguments
);

static void command_run(
    const char *arguments
);

static void command_kill(
    const char *arguments
);

static void command_libtest(
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
        "ls",
        "List files and directories",
        command_ls
    },
    {
        "cd",
        "Change directory",
        command_cd
    },
    {
        "pwd",
        "Show current directory",
        command_pwd
    },
    {
        "cat",
        "Read a text file",
        command_cat
    },
    {
        "mkdir",
        "Create a directory",
        command_mkdir
    },
    {
        "write",
        "Write text: write PATH TEXT",
        command_write
    },
    {
        "ps",
        "List processes",
        command_ps
    },
    {
        "spawn",
        "Create a kernel worker thread",
        command_spawn
    },
    {
        "run",
        "Start a user program: run PATH",
        command_run
    },
    {
        "kill",
        "Terminate a process: kill PID",
        command_kill
    },
    {
        "libtest",
        "Test kernel string and formatting library",
        command_libtest
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

static void command_ls(
    const char *arguments
)
{
    const char *path =
        arguments[0] == '\0' ?
        "." :
        arguments;

    vfs_node_t *directory =
        vfs_open(path);

    if (directory == NULL)
    {
        terminal_write_line(
            "Path not found"
        );
        return;
    }

    if (
        directory->type !=
        VFS_NODE_DIRECTORY
    )
    {
        terminal_write_line(
            "Not a directory"
        );
        return;
    }

    vfs_node_t *child =
        directory->first_child;

    if (child == NULL)
    {
        terminal_write_line("(empty)");
        return;
    }

    while (child != NULL)
    {
        if (
            child->type ==
            VFS_NODE_DIRECTORY
        )
        {
            terminal_write("[DIR]  ");
        }
        else
        {
            terminal_write("[FILE] ");
        }

        terminal_write(child->name);

        if (
            child->type ==
            VFS_NODE_FILE
        )
        {
            char size_text[21];

            uint64_to_string(
                child->size,
                size_text
            );

            terminal_write(" (");
            terminal_write(size_text);
            terminal_write(" bytes)");
        }

        terminal_write_line("");

        child = child->next_sibling;
    }
}

static void command_cd(
    const char *arguments
)
{
    if (arguments[0] == '\0')
    {
        terminal_write_line(
            "Usage: cd PATH"
        );
        return;
    }

    if (!vfs_change_directory(arguments))
    {
        terminal_write_line(
            "Directory not found"
        );
    }
}

static void command_pwd(
    const char *arguments
)
{
    (void)arguments;

    char path[VFS_PATH_MAX];

    if (
        !vfs_get_working_directory(
            path,
            sizeof(path)
        )
    )
    {
        terminal_write_line(
            "Unable to resolve path"
        );
        return;
    }

    terminal_write_line(path);
}

static void command_cat(
    const char *arguments
)
{
    if (arguments[0] == '\0')
    {
        terminal_write_line(
            "Usage: cat PATH"
        );
        return;
    }

    vfs_node_t *file =
        vfs_open(arguments);

    if (file == NULL)
    {
        terminal_write_line(
            "File not found"
        );
        return;
    }

    if (file->type != VFS_NODE_FILE)
    {
        terminal_write_line(
            "Not a file"
        );
        return;
    }

    char buffer[121];
    size_t offset = 0;
    char last_character = '\0';

    while (offset < file->size)
    {
        size_t count =
            vfs_read(
                file,
                offset,
                buffer,
                sizeof(buffer) - 1
            );

        if (count == 0)
        {
            break;
        }

        buffer[count] = '\0';
        last_character =
            buffer[count - 1];

        terminal_write(buffer);
        offset += count;
    }

    if (
        file->size == 0 ||
        last_character != '\n'
    )
    {
        terminal_write_line("");
    }
}

static void command_mkdir(
    const char *arguments
)
{
    if (arguments[0] == '\0')
    {
        terminal_write_line(
            "Usage: mkdir PATH"
        );
        return;
    }

    if (!vfs_make_directory(arguments))
    {
        terminal_write_line(
            "Unable to create directory"
        );
        return;
    }

    terminal_write_line(
        "Directory created"
    );
}

static void command_write(
    const char *arguments
)
{
    char path[VFS_PATH_MAX];
    const char *text;

    if (
        !split_first_argument(
            arguments,
            path,
            sizeof(path),
            &text
        )
    )
    {
        terminal_write_line(
            "Usage: write PATH TEXT"
        );
        return;
    }

    if (!vfs_write_text(path, text))
    {
        terminal_write_line(
            "Unable to write file"
        );
        return;
    }

    terminal_write_line(
        "File written"
    );
}

static void command_ps(
    const char *arguments
)
{
    (void)arguments;
    process_print_all();
}

static void command_spawn(
    const char *arguments
)
{
    (void)arguments;

    if (process_spawn_demo_thread())
    {
        terminal_write_line(
            "Kernel worker thread created"
        );
    }
    else
    {
        terminal_write_line(
            "Unable to create worker thread"
        );
    }
}

static void command_run(
    const char *arguments
)
{
    if (arguments[0] == '\0')
    {
        terminal_write_line(
            "Usage: run PATH"
        );
        return;
    }

    uint64_t pid =
        process_create_user_program(
            arguments
        );

    if (pid == 0)
    {
        terminal_write_line(
            "Unable to load user program"
        );
        return;
    }

    char pid_text[21];
    uint64_to_string(pid, pid_text);

    terminal_write("Started user process ");
    terminal_write_line(pid_text);
}

static void command_kill(
    const char *arguments
)
{
    uint64_t pid;

    if (!parse_uint64(arguments, &pid))
    {
        terminal_write_line(
            "Usage: kill PID"
        );
        return;
    }

    if (!process_terminate(pid))
    {
        terminal_write_line(
            "Unable to terminate process"
        );
        return;
    }

    terminal_write_line(
        "Process terminated"
    );
}

static void command_libtest(
    const char *arguments
)
{
    (void)arguments;

    char copied[32];
    char number[32];
    char formatted[128];
    int64_t parsed_value;

    kstrcpy(copied, "LatterOS");

    bool parsed =
        kparse_i64(
            "-2048",
            &parsed_value
        );

    kitoa(
        parsed_value,
        number,
        10
    );

    int formatted_length =
        ksnprintf(
            formatted,
            sizeof(formatted),
            "formatted: text=%s signed=%lld hex=%08X",
            copied,
            (long long)parsed_value,
            0x2A
        );

    kprintf(
        "strlen(\"%s\") = %zu\n",
        copied,
        kstrlen(copied)
    );

    kprintf(
        "strcmp equal = %d\n",
        kstrcmp(copied, "LatterOS")
    );

    kprintf(
        "atoi result = %s (%s)\n",
        number,
        parsed ? "valid" : "invalid"
    );

    kprintf(
        "%s\n",
        formatted
    );

    kprintf(
        "snprintf length = %d\n",
        formatted_length
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
