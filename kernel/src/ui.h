#ifndef UI_H
#define UI_H

#include <stdbool.h>
#include <stddef.h>
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

bool ui_rectangles_intersect(
    const ui_rect_t *first,
    const ui_rect_t *second
);

ui_rect_t ui_intersection(
    const ui_rect_t *first,
    const ui_rect_t *second
);

ui_rect_t ui_union(
    const ui_rect_t *first,
    const ui_rect_t *second
);

ui_rect_t ui_inset(
    const ui_rect_t *rect,
    int32_t horizontal,
    int32_t vertical
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

void ui_draw_text_ellipsized(
    const char *text,
    const ui_rect_t *rect,
    int32_t x_padding,
    uint32_t color
);

uint32_t ui_text_width(const char *text);
uint32_t ui_text_height(void);

void ui_draw_horizontal_line(
    int32_t x,
    int32_t y,
    uint32_t width,
    uint32_t thickness,
    uint32_t color
);

void ui_draw_vertical_line(
    int32_t x,
    int32_t y,
    uint32_t height,
    uint32_t thickness,
    uint32_t color
);

void ui_draw_cursor(
    int32_t x,
    int32_t y
);

#endif
