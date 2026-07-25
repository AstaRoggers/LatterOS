#include "panic.h"
#include "graphics.h"

#include <stdint.h>

#define PANIC_BACKGROUND 0x8B0000
#define PANIC_FOREGROUND 0xFFFFFF

static void halt_cpu(void)
{
    __asm__ volatile("cli");

    for (;;)
    {
        __asm__ volatile("hlt");
    }
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
            '0' + (value % 10);

        length++;
        value /= 10;
    }

    for (uint32_t i = 0; i < length; i++)
    {
        buffer[i] =
            temporary[length - i - 1];
    }

    buffer[length] = '\0';
}

void kernel_panic(
    const char *message,
    uint64_t exception_number
)
{
    char exception_text[21];

    uint64_to_string(
        exception_number,
        exception_text
    );

    graphics_clear(PANIC_BACKGROUND);

    draw_text(
        "LATTEROS KERNEL PANIC",
        80,
        80,
        PANIC_FOREGROUND
    );

    draw_text(
        "The kernel encountered a fatal error.",
        80,
        110,
        PANIC_FOREGROUND
    );

    draw_text(
        "Exception:",
        80,
        140,
        PANIC_FOREGROUND
    );

    draw_text(
        exception_text,
        176,
        140,
        PANIC_FOREGROUND
    );

    draw_text(
        "Reason:",
        80,
        170,
        PANIC_FOREGROUND
    );

    draw_text(
        message,
        144,
        170,
        PANIC_FOREGROUND
    );

    draw_text(
        "System halted. Restart the machine.",
        80,
        220,
        PANIC_FOREGROUND
    );

    halt_cpu();
}