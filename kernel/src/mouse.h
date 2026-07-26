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

#endif
