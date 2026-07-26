#ifndef MOUSE_H
#define MOUSE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    int32_t x;
    int32_t y;

    bool left_button;
    bool right_button;
    bool middle_button;

    uint64_t packet_count;
} mouse_state_t;

bool mouse_init(void);
bool mouse_is_available(void);
void mouse_get_state(mouse_state_t *state);

void mouse_set_usb_active(bool active);
bool mouse_usb_active(void);
void mouse_handle_usb_boot_report(
    const uint8_t *report,
    uint8_t length
);

#endif
