#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdbool.h>

typedef enum
{
    KEYBOARD_KEY_UNKNOWN,
    KEYBOARD_KEY_ESCAPE,
    KEYBOARD_KEY_TAB,
    KEYBOARD_KEY_ENTER,
    KEYBOARD_KEY_BACKSPACE,
    KEYBOARD_KEY_LEFT_SHIFT,
    KEYBOARD_KEY_RIGHT_SHIFT,
    KEYBOARD_KEY_LEFT_CONTROL,
    KEYBOARD_KEY_RIGHT_CONTROL,
    KEYBOARD_KEY_LEFT_ALT,
    KEYBOARD_KEY_RIGHT_ALT,
    KEYBOARD_KEY_CAPS_LOCK,
    KEYBOARD_KEY_SUPER,
    KEYBOARD_KEY_F1,
    KEYBOARD_KEY_F2,
    KEYBOARD_KEY_F3,
    KEYBOARD_KEY_F4,
    KEYBOARD_KEY_F5,
    KEYBOARD_KEY_F6,
    KEYBOARD_KEY_F7,
    KEYBOARD_KEY_F8,
    KEYBOARD_KEY_F9,
    KEYBOARD_KEY_F10,
    KEYBOARD_KEY_F11,
    KEYBOARD_KEY_F12,
    KEYBOARD_KEY_SPACE,
    KEYBOARD_KEY_D,
    KEYBOARD_KEY_F,
    KEYBOARD_KEY_Q,
    KEYBOARD_KEY_T
} keyboard_key_t;

typedef struct
{
    keyboard_key_t key;
    char character;
    bool pressed;
    bool shift;
    bool control;
    bool alt;
    bool super;
    bool caps_lock;
} keyboard_event_t;

typedef void (*keyboard_character_handler_t)(
    char character
);

typedef bool (*keyboard_event_handler_t)(
    const keyboard_event_t *event
);

void keyboard_init(void);
void keyboard_irq_handler(void);
void keyboard_poll(void);

void keyboard_set_character_handler(
    keyboard_character_handler_t handler
);

void keyboard_set_event_handler(
    keyboard_event_handler_t handler
);

#endif
