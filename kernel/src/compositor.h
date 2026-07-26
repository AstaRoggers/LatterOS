#ifndef COMPOSITOR_H
#define COMPOSITOR_H

#include "ui.h"

#include <stdbool.h>
#include <stdint.h>

typedef void (*compositor_render_function_t)(void);

void compositor_init(
    compositor_render_function_t renderer
);

void compositor_invalidate(
    const ui_rect_t *rectangle
);

void compositor_invalidate_all(void);

bool compositor_has_damage(void);
void compositor_render(void);

uint64_t compositor_frame_count(void);
uint64_t compositor_rectangle_count(void);
uint64_t compositor_pixel_count(void);
uint32_t compositor_average_fps(void);
uint32_t compositor_recent_active_fps(void);
uint64_t compositor_average_render_ms(void);
uint64_t compositor_average_present_ms(void);
void compositor_reset_statistics(void);

#endif
