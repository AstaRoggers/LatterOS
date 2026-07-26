#ifndef UI_H
#define UI_H

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
} ui_rect_t;

bool ui_point_in_rect(
    int32_t x,
    int32_t y,
    const ui_rect_t *rect
);

void ui_fill_rect(
    const ui_rect_t *rect,
    uint32_t color
);

void ui_draw_border(
    const ui_rect_t *rect,
    uint32_t color,
    uint32_t thickness
);

void ui_draw_text(
    const char *text,
    int32_t x,
    int32_t y,
    uint32_t color
);

void ui_draw_text_centered(
    const char *text,
    const ui_rect_t *rect,
    uint32_t color
);

void ui_draw_cursor(
    int32_t x,
    int32_t y
);

#endif
