#ifndef WINDOW_SURFACE_H
#define WINDOW_SURFACE_H

#include "ui.h"

#include <stdbool.h>
#include <stdint.h>

#define WINDOW_SURFACE_COUNT 8U
#define WINDOW_SURFACE_MAX_WIDTH 1280U
#define WINDOW_SURFACE_MAX_HEIGHT 800U

typedef struct
{
    bool valid;
    uint32_t width;
    uint32_t height;
    uint64_t captures;
    uint64_t draws;
    uint64_t scaled_draws;
    uint64_t capture_failures;
} window_surface_info_t;

void window_surface_init(void);
void window_surface_invalidate(uint32_t window_id);
void window_surface_invalidate_all(void);

bool window_surface_capture(
    uint32_t window_id,
    const ui_rect_t *bounds
);

bool window_surface_draw(
    uint32_t window_id,
    const ui_rect_t *bounds
);

bool window_surface_draw_scaled(
    uint32_t window_id,
    const ui_rect_t *bounds
);

bool window_surface_valid_for(
    uint32_t window_id,
    uint32_t width,
    uint32_t height
);

void window_surface_get_info(
    uint32_t window_id,
    window_surface_info_t *information
);

uint64_t window_surface_total_captures(void);
uint64_t window_surface_total_draws(void);
uint64_t window_surface_total_scaled_draws(void);

#endif
