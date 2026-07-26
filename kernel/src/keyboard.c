#include "keyboard.h"

#include "io.h"
#include "terminal.h"

#include <stdbool.h>
#include <stdint.h>

#define PS2_STATUS_PORT      0x64
#define PS2_DATA_PORT        0x60
#define PS2_OUTPUT_FULL      0x01
#define PS2_AUXILIARY_DATA   0x20

#define SCANCODE_LEFT_SHIFT_PRESS   0x2A
#define SCANCODE_RIGHT_SHIFT_PRESS  0x36
#define SCANCODE_LEFT_SHIFT_RELEASE 0xAA
#define SCANCODE_RIGHT_SHIFT_RELEASE 0xB6
#define SCANCODE_CAPS_LOCK_PRESS    0x3A
#define SCANCODE_CAPS_LOCK_RELEASE  0xBA

static const char keymap[128] =
{
    0,
    27,
    '1','2','3','4','5','6','7','8','9','0',
    '-','=',
    '\b',
    '\t',

    'q','w','e','r','t','y','u','i','o','p',
    '[',']',

    '\n',

    0,

    'a','s','d','f','g','h','j','k','l',
    ';','\'','`',

    0,

    '\\',

    'z','x','c','v','b','n','m',
    ',', '.', '/',

    0,
    '*',
    0,
    ' ',

    0
};

static const char shifted_keymap[128] =
{
    0,
    27,
    '!','@','#','$','%','^','&','*','(',')',
    '_','+',
    '\b',
    '\t',

    'Q','W','E','R','T','Y','U','I','O','P',
    '{','}',

    '\n',

    0,

    'A','S','D','F','G','H','J','K','L',
    ':','"','~',

    0,

    '|',

    'Z','X','C','V','B','N','M',
    '<', '>', '?',

    0,
    '*',
    0,
    ' ',

    0
};

static bool left_shift;
static bool right_shift;
static bool caps_lock;
static bool caps_key_down;

static bool character_is_letter(char character)
{
    return
        character >= 'a' &&
        character <= 'z';
}

static void keyboard_process_scancode(
    uint8_t scancode
)
{
    if (scancode == SCANCODE_LEFT_SHIFT_PRESS)
    {
        left_shift = true;
        return;
    }

    if (scancode == SCANCODE_RIGHT_SHIFT_PRESS)
    {
        right_shift = true;
        return;
    }

    if (scancode == SCANCODE_LEFT_SHIFT_RELEASE)
    {
        left_shift = false;
        return;
    }

    if (scancode == SCANCODE_RIGHT_SHIFT_RELEASE)
    {
        right_shift = false;
        return;
    }

    if (scancode == SCANCODE_CAPS_LOCK_PRESS)
    {
        if (!caps_key_down)
        {
            caps_lock = !caps_lock;
            caps_key_down = true;
        }

        return;
    }

    if (scancode == SCANCODE_CAPS_LOCK_RELEASE)
    {
        caps_key_down = false;
        return;
    }

    if (scancode & 0x80)
    {
        return;
    }

    bool shift =
        left_shift || right_shift;

    char character = keymap[scancode];

    if (character_is_letter(character))
    {
        if (caps_lock != shift)
        {
            character =
                (char)(
                    character - 'a' + 'A'
                );
        }
    }
    else if (shift)
    {
        character =
            shifted_keymap[scancode];
    }

    if (character != 0)
    {
        terminal_put_character(
            character
        );
    }
}

static void keyboard_read_data(void)
{
    for (;;)
    {
        uint8_t status =
            inb(PS2_STATUS_PORT);

        if (!(status & PS2_OUTPUT_FULL))
        {
            return;
        }

        if (status & PS2_AUXILIARY_DATA)
        {
            return;
        }

        keyboard_process_scancode(
            inb(PS2_DATA_PORT)
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
    left_shift = false;
    right_shift = false;
    caps_lock = false;
    caps_key_down = false;
}
