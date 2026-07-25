#ifndef GRAPHICS_H
#define GRAPHICS_H

#include <stdint.h>

void graphics_init(
    uint32_t *framebuffer,
    uint64_t pitch,
    uint64_t width,
    uint64_t height
);

void graphics_clear(uint32_t color);

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