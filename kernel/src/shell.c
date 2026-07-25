#include "shell.h"

#include "terminal.h"
#include "timer.h"

#include <stdbool.h>
#include <stdint.h>

typedef void (*command_function_t)(
    const char *arguments
);

typedef struct
{
    const char *name;
    const char *description;
    command_function_t function;
} shell_command_t;

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
        uint32_t i = 0;
        i < COMMAND_COUNT;
        i++
    )
    {
        terminal_write(commands[i].name);
        terminal_write(" - ");

        terminal_write_line(
            commands[i].description
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

    terminal_write(
        "Timer ticks: "
    );

    terminal_write_line(text);
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
        uint32_t i = 0;
        i < COMMAND_COUNT;
        i++
    )
    {
        if (
            strings_equal(
                command_name,
                commands[i].name
            )
        )
        {
            commands[i].function(
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
