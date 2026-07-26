#include "ui.h"

#include "graphics.h"

#include <stddef.h>
#include <stdint.h>

static uint32_t text_width(const char *text)
{
    if (text == NULL)
    {
        return 0;
    }

    uint32_t length = 0;

    while (text[length] != '\0')
    {
        length++;
    }

    return length * 8;
}

bool ui_point_in_rect(
    int32_t x,
    int32_t y,
    const ui_rect_t *rect
)
{
    if (rect == NULL)
    {
        return false;
    }

    return
        x >= rect->x &&
        y >= rect->y &&
        x < rect->x + (int32_t)rect->width &&
        y < rect->y + (int32_t)rect->height;
}

void ui_fill_rect(
    const ui_rect_t *rect,
    uint32_t color
)
{
    if (rect == NULL)
    {
        return;
    }

    draw_rectangle(
        (uint32_t)rect->x,
        (uint32_t)rect->y,
        rect->width,
        rect->height,
        color
    );
}

void ui_draw_border(
    const ui_rect_t *rect,
    uint32_t color,
    uint32_t thickness
)
{
    if (
        rect == NULL ||
        thickness == 0 ||
        rect->width < thickness * 2 ||
        rect->height < thickness * 2
    )
    {
        return;
    }

    ui_rect_t top = {
        rect->x,
        rect->y,
        rect->width,
        thickness
    };

    ui_rect_t bottom = {
        rect->x,
        rect->y + (int32_t)rect->height -
            (int32_t)thickness,
        rect->width,
        thickness
    };

    ui_rect_t left = {
        rect->x,
        rect->y,
        thickness,
        rect->height
    };

    ui_rect_t right = {
        rect->x + (int32_t)rect->width -
            (int32_t)thickness,
        rect->y,
        thickness,
        rect->height
    };

    ui_fill_rect(&top, color);
    ui_fill_rect(&bottom, color);
    ui_fill_rect(&left, color);
    ui_fill_rect(&right, color);
}

void ui_draw_text(
    const char *text,
    int32_t x,
    int32_t y,
    uint32_t color
)
{
    if (text == NULL)
    {
        return;
    }

    draw_text(
        text,
        (uint32_t)x,
        (uint32_t)y,
        color
    );
}

void ui_draw_text_centered(
    const char *text,
    const ui_rect_t *rect,
    uint32_t color
)
{
    if (
        text == NULL ||
        rect == NULL
    )
    {
        return;
    }

    uint32_t width = text_width(text);

    int32_t x = rect->x;

    if (rect->width > width)
    {
        x += (int32_t)(
            (rect->width - width) / 2
        );
    }

    int32_t y = rect->y;

    if (rect->height > 8)
    {
        y += (int32_t)(
            (rect->height - 8) / 2
        );
    }

    ui_draw_text(
        text,
        x,
        y,
        color
    );
}

void ui_draw_cursor(
    int32_t x,
    int32_t y
)
{
    static const uint16_t cursor_rows[16] = {
        0x8000,
        0xC000,
        0xE000,
        0xF000,
        0xF800,
        0xFC00,
        0xFE00,
        0xFF00,
        0xFF80,
        0xF800,
        0xD800,
        0x8C00,
        0x0C00,
        0x0600,
        0x0600,
        0x0000
    };

    for (uint32_t row = 0; row < 16; row++)
    {
        for (uint32_t column = 0; column < 16; column++)
        {
            if (
                cursor_rows[row] &
                (uint16_t)(0x8000U >> column)
            )
            {
                draw_pixel(
                    (uint32_t)(x + (int32_t)column),
                    (uint32_t)(y + (int32_t)row),
                    0xFFFFFF
                );
            }
        }
    }

    draw_pixel((uint32_t)x, (uint32_t)y, 0x000000);
    draw_pixel((uint32_t)(x + 1), (uint32_t)(y + 1), 0x000000);
    draw_pixel((uint32_t)(x + 2), (uint32_t)(y + 2), 0x000000);
}
