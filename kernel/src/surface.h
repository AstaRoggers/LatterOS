#ifndef SURFACE_H
#define SURFACE_H

#include "ui.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct
{
    uint32_t *pixels;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
} surface_t;

bool surface_init(
    surface_t *surface,
    uint32_t *pixels,
    uint32_t width,
    uint32_t height,
    uint32_t stride
);

bool surface_valid(const surface_t *surface);
void surface_clear(surface_t *surface, uint32_t color);
void surface_fill_rect(
    surface_t *surface,
    const ui_rect_t *rectangle,
    uint32_t color
);

bool surface_copy_rect(
    surface_t *destination,
    int32_t destination_x,
    int32_t destination_y,
    const surface_t *source,
    const ui_rect_t *source_rectangle
);

bool surface_alpha_blit(
    surface_t *destination,
    int32_t destination_x,
    int32_t destination_y,
    const surface_t *source,
    const ui_rect_t *source_rectangle,
    uint8_t opacity
);

bool surface_scale_nearest(
    surface_t *destination,
    const ui_rect_t *destination_rectangle,
    const surface_t *source,
    const ui_rect_t *source_rectangle
);

uint64_t surface_checksum(const surface_t *surface);

#endif
