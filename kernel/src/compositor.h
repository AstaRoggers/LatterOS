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

const char *compositor_display_backend(void);
uint32_t compositor_scanout_buffer_count(void);
uint64_t compositor_dropped_frame_count(void);
uint64_t compositor_vsync_count(void);
bool compositor_triple_buffered(void);

void compositor_reset_statistics(void);

#endif
