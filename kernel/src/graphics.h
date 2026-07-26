#ifndef GRAPHICS_H
#define GRAPHICS_H

#include <stdbool.h>
#include <stdint.h>

void graphics_init(
    uint32_t *framebuffer,
    uint64_t pitch,
    uint64_t width,
    uint64_t height
);

void graphics_set_deferred(bool deferred);
bool graphics_is_deferred(void);

void graphics_present(void);
void graphics_present_rectangle(
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height
);

void graphics_set_clip(
    int32_t x,
    int32_t y,
    uint32_t width,
    uint32_t height
);

void graphics_reset_clip(void);

void graphics_get_clip(
    int32_t *x,
    int32_t *y,
    int32_t *right,
    int32_t *bottom
);

void graphics_clear(uint32_t color);

uint32_t graphics_width(void);
uint32_t graphics_height(void);

uint32_t graphics_get_pixel(
    uint32_t x,
    uint32_t y
);

void draw_pixel(
    uint32_t x,
    uint32_t y,
    uint32_t color
);

void draw_rectangle(
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height,
    uint32_t color
);

void draw_character(
    char character,
    uint32_t x,
    uint32_t y,
    uint32_t color
);

void draw_text(
    const char *text,
    uint32_t x,
    uint32_t y,
    uint32_t color
);

#endif
