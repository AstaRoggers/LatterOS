#include "keyboard.h"

#include "io.h"
#include "terminal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PS2_STATUS_PORT      0x64
#define PS2_DATA_PORT        0x60
#define PS2_OUTPUT_FULL      0x01
#define PS2_AUXILIARY_DATA   0x20

#define SCANCODE_EXTENDED    0xE0

#define SCANCODE_ESCAPE      0x01
#define SCANCODE_BACKSPACE   0x0E
#define SCANCODE_TAB         0x0F
#define SCANCODE_ENTER       0x1C
#define SCANCODE_LEFT_CTRL   0x1D
#define SCANCODE_Q           0x10
#define SCANCODE_T           0x14
#define SCANCODE_D           0x20
#define SCANCODE_F           0x21
#define SCANCODE_SPACE       0x39
#define SCANCODE_LEFT_SHIFT  0x2A
#define SCANCODE_RIGHT_SHIFT 0x36
#define SCANCODE_LEFT_ALT    0x38
#define SCANCODE_CAPS_LOCK   0x3A
#define SCANCODE_F1          0x3B
#define SCANCODE_F2          0x3C
#define SCANCODE_F3          0x3D
#define SCANCODE_F4          0x3E
#define SCANCODE_F5          0x3F
#define SCANCODE_F6          0x40
#define SCANCODE_F7          0x41
#define SCANCODE_F8          0x42
#define SCANCODE_F9          0x43
#define SCANCODE_F10         0x44
#define SCANCODE_F11         0x57
#define SCANCODE_F12         0x58
#define SCANCODE_SUPER       0x5B

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
static bool left_control;
static bool right_control;
static bool left_alt;
static bool right_alt;
static bool super_key;
static bool caps_lock;
static bool caps_key_down;
static bool extended_scancode;

static keyboard_character_handler_t character_handler;
static keyboard_event_handler_t event_handler;

static bool usb_input_active;
static uint8_t usb_previous_modifiers;
static uint8_t usb_previous_keys[6];

static bool character_is_letter(char character)
{
    return
        character >= 'a' &&
        character <= 'z';
}

static keyboard_key_t keyboard_key_from_scancode(
    uint8_t code,
    bool extended
)
{
    if (extended)
    {
        if (code == SCANCODE_LEFT_CTRL)
        {
            return KEYBOARD_KEY_RIGHT_CONTROL;
        }

        if (code == SCANCODE_LEFT_ALT)
        {
            return KEYBOARD_KEY_RIGHT_ALT;
        }

        if (code == SCANCODE_SUPER)
        {
            return KEYBOARD_KEY_SUPER;
        }

        return KEYBOARD_KEY_UNKNOWN;
    }

    switch (code)
    {
        case SCANCODE_ESCAPE:
            return KEYBOARD_KEY_ESCAPE;

        case SCANCODE_TAB:
            return KEYBOARD_KEY_TAB;

        case SCANCODE_ENTER:
            return KEYBOARD_KEY_ENTER;

        case SCANCODE_BACKSPACE:
            return KEYBOARD_KEY_BACKSPACE;

        case SCANCODE_LEFT_SHIFT:
            return KEYBOARD_KEY_LEFT_SHIFT;

        case SCANCODE_RIGHT_SHIFT:
            return KEYBOARD_KEY_RIGHT_SHIFT;

        case SCANCODE_LEFT_CTRL:
            return KEYBOARD_KEY_LEFT_CONTROL;

        case SCANCODE_LEFT_ALT:
            return KEYBOARD_KEY_LEFT_ALT;

        case SCANCODE_CAPS_LOCK:
            return KEYBOARD_KEY_CAPS_LOCK;

        case SCANCODE_F1:
            return KEYBOARD_KEY_F1;

        case SCANCODE_F2:
            return KEYBOARD_KEY_F2;

        case SCANCODE_F3:
            return KEYBOARD_KEY_F3;

        case SCANCODE_F4:
            return KEYBOARD_KEY_F4;

        case SCANCODE_F5:
            return KEYBOARD_KEY_F5;

        case SCANCODE_F6:
            return KEYBOARD_KEY_F6;

        case SCANCODE_F7:
            return KEYBOARD_KEY_F7;

        case SCANCODE_F8:
            return KEYBOARD_KEY_F8;

        case SCANCODE_F9:
            return KEYBOARD_KEY_F9;

        case SCANCODE_F10:
            return KEYBOARD_KEY_F10;

        case SCANCODE_F11:
            return KEYBOARD_KEY_F11;

        case SCANCODE_F12:
            return KEYBOARD_KEY_F12;

        case SCANCODE_SPACE:
            return KEYBOARD_KEY_SPACE;

        case SCANCODE_D:
            return KEYBOARD_KEY_D;

        case SCANCODE_F:
            return KEYBOARD_KEY_F;

        case SCANCODE_Q:
            return KEYBOARD_KEY_Q;

        case SCANCODE_T:
            return KEYBOARD_KEY_T;

        default:
            return KEYBOARD_KEY_UNKNOWN;
    }
}

static void update_modifier(
    keyboard_key_t key,
    bool pressed
)
{
    switch (key)
    {
        case KEYBOARD_KEY_LEFT_SHIFT:
            left_shift = pressed;
            break;

        case KEYBOARD_KEY_RIGHT_SHIFT:
            right_shift = pressed;
            break;

        case KEYBOARD_KEY_LEFT_CONTROL:
            left_control = pressed;
            break;

        case KEYBOARD_KEY_RIGHT_CONTROL:
            right_control = pressed;
            break;

        case KEYBOARD_KEY_LEFT_ALT:
            left_alt = pressed;
            break;

        case KEYBOARD_KEY_RIGHT_ALT:
            right_alt = pressed;
            break;

        case KEYBOARD_KEY_SUPER:
            super_key = pressed;
            break;

        default:
            break;
    }
}

static char translated_character(
    uint8_t code,
    bool pressed,
    bool extended
)
{
    if (
        !pressed ||
        extended ||
        code >= 128
    )
    {
        return 0;
    }

    bool shift =
        left_shift || right_shift;

    char character = keymap[code];

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
        character = shifted_keymap[code];
    }

    return character;
}

static void keyboard_process_scancode(
    uint8_t scancode
)
{
    if (scancode == SCANCODE_EXTENDED)
    {
        extended_scancode = true;
        return;
    }

    bool extended = extended_scancode;
    extended_scancode = false;

    bool pressed =
        (scancode & 0x80U) == 0;

    uint8_t code =
        scancode & 0x7FU;

    keyboard_key_t key =
        keyboard_key_from_scancode(
            code,
            extended
        );

    update_modifier(key, pressed);

    if (key == KEYBOARD_KEY_CAPS_LOCK)
    {
        if (pressed && !caps_key_down)
        {
            caps_lock = !caps_lock;
            caps_key_down = true;
        }
        else if (!pressed)
        {
            caps_key_down = false;
        }
    }

    char character =
        translated_character(
            code,
            pressed,
            extended
        );

    keyboard_event_t event = {
        .key = key,
        .character = character,
        .pressed = pressed,
        .shift = left_shift || right_shift,
        .control = left_control || right_control,
        .alt = left_alt || right_alt,
        .super = super_key,
        .caps_lock = caps_lock
    };

    bool consumed = false;

    if (event_handler != NULL)
    {
        consumed = event_handler(&event);
    }

    if (
        pressed &&
        character != 0 &&
        !consumed &&
        !event.control &&
        !event.alt &&
        !event.super &&
        character_handler != NULL
    )
    {
        character_handler(character);
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

        uint8_t scancode =
            inb(PS2_DATA_PORT);

        if (!usb_input_active)
        {
            keyboard_process_scancode(
                scancode
            );
        }
    }
}

static bool usb_report_contains(
    const uint8_t report[8],
    uint8_t usage
)
{
    for (
        uint8_t index = 2;
        index < 8;
        index++
    )
    {
        if (report[index] == usage)
        {
            return true;
        }
    }

    return false;
}

static bool previous_report_contains(
    uint8_t usage
)
{
    for (
        uint8_t index = 0;
        index < 6;
        index++
    )
    {
        if (usb_previous_keys[index] == usage)
        {
            return true;
        }
    }

    return false;
}

static keyboard_key_t usb_usage_key(
    uint8_t usage
)
{
    switch (usage)
    {
        case 0x28:
            return KEYBOARD_KEY_ENTER;

        case 0x29:
            return KEYBOARD_KEY_ESCAPE;

        case 0x2A:
            return KEYBOARD_KEY_BACKSPACE;

        case 0x2B:
            return KEYBOARD_KEY_TAB;

        case 0x2C:
            return KEYBOARD_KEY_SPACE;

        case 0x39:
            return KEYBOARD_KEY_CAPS_LOCK;

        case 0x3A:
            return KEYBOARD_KEY_F1;

        case 0x3B:
            return KEYBOARD_KEY_F2;

        case 0x3C:
            return KEYBOARD_KEY_F3;

        case 0x3D:
            return KEYBOARD_KEY_F4;

        case 0x3E:
            return KEYBOARD_KEY_F5;

        case 0x3F:
            return KEYBOARD_KEY_F6;

        case 0x40:
            return KEYBOARD_KEY_F7;

        case 0x41:
            return KEYBOARD_KEY_F8;

        case 0x42:
            return KEYBOARD_KEY_F9;

        case 0x43:
            return KEYBOARD_KEY_F10;

        case 0x44:
            return KEYBOARD_KEY_F11;

        case 0x45:
            return KEYBOARD_KEY_F12;

        case 0x07:
            return KEYBOARD_KEY_D;

        case 0x09:
            return KEYBOARD_KEY_F;

        case 0x14:
            return KEYBOARD_KEY_Q;

        case 0x17:
            return KEYBOARD_KEY_T;

        default:
            return KEYBOARD_KEY_UNKNOWN;
    }
}

static char usb_usage_character(
    uint8_t usage
)
{
    bool shift =
        left_shift || right_shift;

    if (
        usage >= 0x04 &&
        usage <= 0x1D
    )
    {
        char character =
            (char)(
                'a' +
                (usage - 0x04)
            );

        if (caps_lock != shift)
        {
            character =
                (char)(
                    character - 'a' + 'A'
                );
        }

        return character;
    }

    static const char normal_digits[10] = {
        '1','2','3','4','5',
        '6','7','8','9','0'
    };

    static const char shifted_digits[10] = {
        '!','@','#','$','%',
        '^','&','*','(',')'
    };

    if (
        usage >= 0x1E &&
        usage <= 0x27
    )
    {
        uint8_t index =
            (uint8_t)(usage - 0x1E);

        return shift ?
            shifted_digits[index] :
            normal_digits[index];
    }

    switch (usage)
    {
        case 0x28:
            return '\n';

        case 0x2A:
            return '\b';

        case 0x2B:
            return '\t';

        case 0x2C:
            return ' ';

        case 0x2D:
            return shift ? '_' : '-';

        case 0x2E:
            return shift ? '+' : '=';

        case 0x2F:
            return shift ? '{' : '[';

        case 0x30:
            return shift ? '}' : ']';

        case 0x31:
            return shift ? '|' : '\\';

        case 0x33:
            return shift ? ':' : ';';

        case 0x34:
            return shift ? '"' : '\'';

        case 0x35:
            return shift ? '~' : '`';

        case 0x36:
            return shift ? '<' : ',';

        case 0x37:
            return shift ? '>' : '.';

        case 0x38:
            return shift ? '?' : '/';

        default:
            return 0;
    }
}

static void dispatch_usb_event(
    keyboard_key_t key,
    char character,
    bool pressed
)
{
    keyboard_event_t event = {
        .key = key,
        .character = character,
        .pressed = pressed,
        .shift = left_shift || right_shift,
        .control = left_control || right_control,
        .alt = left_alt || right_alt,
        .super = super_key,
        .caps_lock = caps_lock
    };

    bool consumed = false;

    if (event_handler != NULL)
    {
        consumed = event_handler(&event);
    }

    if (
        pressed &&
        character != 0 &&
        !consumed &&
        !event.control &&
        !event.alt &&
        !event.super &&
        character_handler != NULL
    )
    {
        character_handler(character);
    }
}

static keyboard_key_t usb_modifier_key(
    uint8_t bit
)
{
    switch (bit)
    {
        case 0:
            return KEYBOARD_KEY_LEFT_CONTROL;

        case 1:
            return KEYBOARD_KEY_LEFT_SHIFT;

        case 2:
            return KEYBOARD_KEY_LEFT_ALT;

        case 3:
            return KEYBOARD_KEY_SUPER;

        case 4:
            return KEYBOARD_KEY_RIGHT_CONTROL;

        case 5:
            return KEYBOARD_KEY_RIGHT_SHIFT;

        case 6:
            return KEYBOARD_KEY_RIGHT_ALT;

        case 7:
            return KEYBOARD_KEY_SUPER;

        default:
            return KEYBOARD_KEY_UNKNOWN;
    }
}

void keyboard_set_usb_active(bool active)
{
    usb_input_active = active;

    if (!active)
    {
        usb_previous_modifiers = 0;

        for (
            uint8_t index = 0;
            index < 6;
            index++
        )
        {
            usb_previous_keys[index] = 0;
        }
    }
}

bool keyboard_usb_active(void)
{
    return usb_input_active;
}

void keyboard_handle_usb_boot_report(
    const uint8_t report[8]
)
{
    if (
        !usb_input_active ||
        report == NULL
    )
    {
        return;
    }

    uint8_t modifiers = report[0];
    uint8_t changed =
        (uint8_t)(
            modifiers ^
            usb_previous_modifiers
        );

    for (
        uint8_t bit = 0;
        bit < 8;
        bit++
    )
    {
        uint8_t mask =
            (uint8_t)(1U << bit);

        if (!(changed & mask))
        {
            continue;
        }

        keyboard_key_t key =
            usb_modifier_key(bit);

        bool pressed =
            (modifiers & mask) != 0;

        update_modifier(
            key,
            pressed
        );

        dispatch_usb_event(
            key,
            0,
            pressed
        );
    }

    usb_previous_modifiers = modifiers;

    for (
        uint8_t index = 0;
        index < 6;
        index++
    )
    {
        uint8_t usage =
            usb_previous_keys[index];

        if (
            usage == 0 ||
            usb_report_contains(
                report,
                usage
            )
        )
        {
            continue;
        }

        dispatch_usb_event(
            usb_usage_key(usage),
            0,
            false
        );
    }

    for (
        uint8_t index = 2;
        index < 8;
        index++
    )
    {
        uint8_t usage = report[index];

        if (
            usage == 0 ||
            usage == 1 ||
            previous_report_contains(usage)
        )
        {
            continue;
        }

        if (usage == 0x39)
        {
            caps_lock = !caps_lock;
        }

        dispatch_usb_event(
            usb_usage_key(usage),
            usb_usage_character(usage),
            true
        );
    }

    for (
        uint8_t index = 0;
        index < 6;
        index++
    )
    {
        usb_previous_keys[index] =
            report[index + 2];
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
    left_control = false;
    right_control = false;
    left_alt = false;
    right_alt = false;
    super_key = false;
    caps_lock = false;
    caps_key_down = false;
    extended_scancode = false;
    usb_previous_modifiers = 0;

    for (
        uint8_t index = 0;
        index < 6;
        index++
    )
    {
        usb_previous_keys[index] = 0;
    }

    character_handler = terminal_put_character;
    event_handler = NULL;
}

void keyboard_set_character_handler(
    keyboard_character_handler_t handler
)
{
    if (handler == NULL)
    {
        character_handler = terminal_put_character;
        return;
    }

    character_handler = handler;
}

void keyboard_set_event_handler(
    keyboard_event_handler_t handler
)
{
    event_handler = handler;
}
