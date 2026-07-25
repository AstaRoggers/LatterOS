#include "keyboard.h"

#include "io.h"
#include "terminal.h"

#include <stdbool.h>
#include <stdint.h>

#define PS2_DATA_PORT        0x60
#define PS2_STATUS_PORT      0x64
#define PS2_COMMAND_PORT     0x64

#define PS2_OUTPUT_FULL      0x01
#define PS2_INPUT_FULL       0x02

#define PS2_DISABLE_PORT1    0xAD
#define PS2_DISABLE_PORT2    0xA7
#define PS2_ENABLE_PORT1     0xAE

#define PS2_READ_CONFIG      0x20
#define PS2_WRITE_CONFIG     0x60

#define PS2_CONFIG_IRQ1      0x01
#define PS2_CONFIG_IRQ12     0x02
#define PS2_CONFIG_CLOCK1    0x10
#define PS2_CONFIG_CLOCK2    0x20
#define PS2_CONFIG_TRANSLATE 0x40

#define KEYBOARD_ENABLE_SCAN 0xF4
#define KEYBOARD_ACK         0xFA

#define PS2_TIMEOUT 1000000

static const char keymap[128] =
{
    0,
    27,
    '1', '2', '3', '4', '5', '6',
    '7', '8', '9', '0',
    '-', '=',
    '\b',
    '\t',

    'q', 'w', 'e', 'r', 't', 'y',
    'u', 'i', 'o', 'p',
    '[', ']',

    '\n',

    0,

    'a', 's', 'd', 'f', 'g', 'h',
    'j', 'k', 'l',
    ';', '\'', '`',

    0,

    '\\',

    'z', 'x', 'c', 'v', 'b', 'n',
    'm',
    ',', '.', '/',

    0,
    '*',
    0,
    ' '
};

static bool ps2_wait_input_empty(void)
{
    for (
        uint32_t timeout = 0;
        timeout < PS2_TIMEOUT;
        timeout++
    )
    {
        if (
            !(inb(PS2_STATUS_PORT) &
            PS2_INPUT_FULL)
        )
        {
            return true;
        }

        __asm__ volatile("pause");
    }

    return false;
}

static bool ps2_wait_output_full(void)
{
    for (
        uint32_t timeout = 0;
        timeout < PS2_TIMEOUT;
        timeout++
    )
    {
        if (
            inb(PS2_STATUS_PORT) &
            PS2_OUTPUT_FULL
        )
        {
            return true;
        }

        __asm__ volatile("pause");
    }

    return false;
}

static bool ps2_send_controller_command(
    uint8_t command
)
{
    if (!ps2_wait_input_empty())
    {
        return false;
    }

    outb(
        PS2_COMMAND_PORT,
        command
    );

    return true;
}

static bool ps2_send_data(uint8_t value)
{
    if (!ps2_wait_input_empty())
    {
        return false;
    }

    outb(
        PS2_DATA_PORT,
        value
    );

    return true;
}

static bool ps2_read_data(uint8_t *value)
{
    if (!ps2_wait_output_full())
    {
        return false;
    }

    *value = inb(PS2_DATA_PORT);

    return true;
}

static void ps2_flush_output(void)
{
    while (
        inb(PS2_STATUS_PORT) &
        PS2_OUTPUT_FULL
    )
    {
        (void)inb(PS2_DATA_PORT);
    }
}

static void keyboard_process_scancode(
    uint8_t scancode
)
{
    /*
     * Ignore key-release events.
     */
    if (scancode & 0x80)
    {
        return;
    }

    if (scancode >= 128)
    {
        return;
    }

    char character =
        keymap[scancode];

    if (character != 0)
    {
        terminal_put_character(
            character
        );
    }
}

static void keyboard_read_data(void)
{
    while (
        inb(PS2_STATUS_PORT) &
        PS2_OUTPUT_FULL
    )
    {
        uint8_t scancode =
            inb(PS2_DATA_PORT);

        keyboard_process_scancode(
            scancode
        );
    }
}

void keyboard_irq_handler(void)
{
    keyboard_read_data();
}

void keyboard_poll(void)
{
    keyboard_read_data();
}

void keyboard_init(void)
{
    /*
     * Disable both PS/2 ports while configuring the controller.
     */
    ps2_send_controller_command(
        PS2_DISABLE_PORT1
    );

    ps2_send_controller_command(
        PS2_DISABLE_PORT2
    );

    ps2_flush_output();

    /*
     * Read the controller configuration byte.
     */
    if (
        !ps2_send_controller_command(
            PS2_READ_CONFIG
        )
    )
    {
        return;
    }

    uint8_t configuration;

    if (!ps2_read_data(&configuration))
    {
        return;
    }

    /*
     * Enable first-port interrupts.
     * Disable second-port interrupts.
     * Enable the first-port clock.
     * Keep scan-code translation enabled for the keymap.
     */
    configuration |=
        PS2_CONFIG_IRQ1;

    configuration &=
        (uint8_t)~PS2_CONFIG_IRQ12;

    configuration &=
        (uint8_t)~PS2_CONFIG_CLOCK1;

    configuration |=
        PS2_CONFIG_CLOCK2;

    configuration |=
        PS2_CONFIG_TRANSLATE;

    if (
        !ps2_send_controller_command(
            PS2_WRITE_CONFIG
        )
    )
    {
        return;
    }

    if (!ps2_send_data(configuration))
    {
        return;
    }

    /*
     * Re-enable the keyboard port.
     */
    ps2_send_controller_command(
        PS2_ENABLE_PORT1
    );

    /*
     * Explicitly enable keyboard scanning.
     */
    if (
        ps2_send_data(
            KEYBOARD_ENABLE_SCAN
        )
    )
    {
        uint8_t response;

        if (ps2_read_data(&response))
        {
            /*
             * Ignore unexpected responses for now.
             */
            (void)response;
        }
    }
}