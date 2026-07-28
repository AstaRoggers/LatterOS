#ifndef WINDOW_ANIMATION_H
#define WINDOW_ANIMATION_H

#include "ui.h"

#include <stdbool.h>
#include <stdint.h>

#define WINDOW_ANIMATION_COUNT 8U

typedef enum
{
    WINDOW_ANIMATION_NONE,
    WINDOW_ANIMATION_OPEN,
    WINDOW_ANIMATION_MINIMIZE,
    WINDOW_ANIMATION_MAXIMIZE,
    WINDOW_ANIMATION_RESTORE,
    WINDOW_ANIMATION_CLOSE
} window_animation_type_t;

void window_animation_init(void);

bool window_animation_start(
    uint32_t window_id,
    window_animation_type_t type,
    const ui_rect_t *from,
    const ui_rect_t *to,
    uint32_t duration_ms
);

void window_animation_cancel(uint32_t window_id);
bool window_animation_active(uint32_t window_id);
window_animation_type_t window_animation_type(uint32_t window_id);

bool window_animation_bounds(
    uint32_t window_id,
    ui_rect_t *bounds
);

bool window_animation_step(
    uint32_t window_id,
    ui_rect_t *previous,
    ui_rect_t *current,
    bool *finished
);

#endif
