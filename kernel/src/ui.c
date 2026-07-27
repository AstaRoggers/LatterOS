#include "ui.h"

#include "graphics.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UI_GLYPH_WIDTH 8U
#define UI_GLYPH_HEIGHT 8U

static int64_t rect_right(const ui_rect_t *rect)
{
    return rect == NULL ? 0 :
        (int64_t)rect->x + (int64_t)rect->width;
}

static int64_t rect_bottom(const ui_rect_t *rect)
{
    return rect == NULL ? 0 :
        (int64_t)rect->y + (int64_t)rect->height;
}

static size_t text_length(const char *text)
{
    size_t length = 0;

    if (text == NULL)
    {
        return 0;
    }

    while (text[length] != '\0')
    {
        length++;
    }

    return length;
}

bool ui_point_in_rect(
    int32_t x,
    int32_t y,
    const ui_rect_t *rect
)
{
    if (rect == NULL || rect->width == 0 || rect->height == 0)
    {
        return false;
    }

    return
        x >= rect->x &&
        y >= rect->y &&
        (int64_t)x < rect_right(rect) &&
        (int64_t)y < rect_bottom(rect);
}

bool ui_rectangles_intersect(
    const ui_rect_t *first,
    const ui_rect_t *second
)
{
    if (
        first == NULL ||
        second == NULL ||
        first->width == 0 ||
        first->height == 0 ||
        second->width == 0 ||
        second->height == 0
    )
    {
        return false;
    }

    return
        first->x < rect_right(second) &&
        second->x < rect_right(first) &&
        first->y < rect_bottom(second) &&
        second->y < rect_bottom(first);
}

ui_rect_t ui_intersection(
    const ui_rect_t *first,
    const ui_rect_t *second
)
{
    ui_rect_t empty = { 0, 0, 0, 0 };

    if (!ui_rectangles_intersect(first, second))
    {
        return empty;
    }

    int32_t left = first->x > second->x ?
        first->x : second->x;

    int32_t top = first->y > second->y ?
        first->y : second->y;

    int64_t first_right = rect_right(first);
    int64_t second_right = rect_right(second);
    int64_t first_bottom = rect_bottom(first);
    int64_t second_bottom = rect_bottom(second);

    int64_t right = first_right < second_right ?
        first_right : second_right;

    int64_t bottom = first_bottom < second_bottom ?
        first_bottom : second_bottom;

    ui_rect_t rectangle = {
        .x = left,
        .y = top,
        .width = (uint32_t)(right - left),
        .height = (uint32_t)(bottom - top)
    };

    return rectangle;
}

ui_rect_t ui_union(
    const ui_rect_t *first,
    const ui_rect_t *second
)
{
    if (first == NULL)
    {
        return second == NULL ?
            (ui_rect_t){ 0, 0, 0, 0 } : *second;
    }

    if (second == NULL)
    {
        return *first;
    }

    int32_t left = first->x < second->x ?
        first->x : second->x;

    int32_t top = first->y < second->y ?
        first->y : second->y;

    int64_t first_right = rect_right(first);
    int64_t second_right = rect_right(second);
    int64_t first_bottom = rect_bottom(first);
    int64_t second_bottom = rect_bottom(second);

    int64_t right = first_right > second_right ?
        first_right : second_right;

    int64_t bottom = first_bottom > second_bottom ?
        first_bottom : second_bottom;

    ui_rect_t rectangle = {
        .x = left,
        .y = top,
        .width = (uint32_t)(right - left),
        .height = (uint32_t)(bottom - top)
    };

    return rectangle;
}

ui_rect_t ui_inset(
    const ui_rect_t *rect,
    int32_t horizontal,
    int32_t vertical
)
{
    if (rect == NULL)
    {
        return (ui_rect_t){ 0, 0, 0, 0 };
    }

    int64_t width =
        (int64_t)rect->width -
        (int64_t)horizontal * 2;

    int64_t height =
        (int64_t)rect->height -
        (int64_t)vertical * 2;

    ui_rect_t result = {
        .x = rect->x + horizontal,
        .y = rect->y + vertical,
        .width = width > 0 ? (uint32_t)width : 0,
        .height = height > 0 ? (uint32_t)height : 0
    };

    return result;
}

void ui_fill_rect(
    const ui_rect_t *rect,
    uint32_t color
)
{
    if (rect == NULL || rect->width == 0 || rect->height == 0)
    {
        return;
    }

    int64_t left = rect->x;
    int64_t top = rect->y;
    int64_t right = rect_right(rect);
    int64_t bottom = rect_bottom(rect);

    if (right <= 0 || bottom <= 0)
    {
        return;
    }

    uint32_t screen_width = graphics_width();
    uint32_t screen_height = graphics_height();

    if (left < 0)
    {
        left = 0;
    }

    if (top < 0)
    {
        top = 0;
    }

    if (right > (int64_t)screen_width)
    {
        right = screen_width;
    }

    if (bottom > (int64_t)screen_height)
    {
        bottom = screen_height;
    }

    if (right <= left || bottom <= top)
    {
        return;
    }

    draw_rectangle(
        (uint32_t)left,
        (uint32_t)top,
        (uint32_t)(right - left),
        (uint32_t)(bottom - top),
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
        rect->width == 0 ||
        rect->height == 0
    )
    {
        return;
    }

    if (thickness * 2U > rect->width)
    {
        thickness = rect->width / 2U;
    }

    if (thickness * 2U > rect->height)
    {
        thickness = rect->height / 2U;
    }

    if (thickness == 0)
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
    if (
        text == NULL ||
        x >= (int32_t)graphics_width() ||
        y >= (int32_t)graphics_height()
    )
    {
        return;
    }

    if (x < 0 || y < 0)
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

uint32_t ui_text_width(const char *text)
{
    size_t length = text_length(text);

    if (length > UINT32_MAX / UI_GLYPH_WIDTH)
    {
        return UINT32_MAX;
    }

    return (uint32_t)length * UI_GLYPH_WIDTH;
}

uint32_t ui_text_height(void)
{
    return UI_GLYPH_HEIGHT;
}

void ui_draw_text_centered(
    const char *text,
    const ui_rect_t *rect,
    uint32_t color
)
{
    if (text == NULL || rect == NULL)
    {
        return;
    }

    uint32_t width = ui_text_width(text);
    int32_t x = rect->x;
    int32_t y = rect->y;

    if (rect->width > width)
    {
        x += (int32_t)((rect->width - width) / 2U);
    }

    if (rect->height > UI_GLYPH_HEIGHT)
    {
        y += (int32_t)((rect->height - UI_GLYPH_HEIGHT) / 2U);
    }

    ui_draw_text(text, x, y, color);
}

void ui_draw_text_ellipsized(
    const char *text,
    const ui_rect_t *rect,
    int32_t x_padding,
    uint32_t color
)
{
    if (text == NULL || rect == NULL || rect->width == 0)
    {
        return;
    }

    int32_t available =
        (int32_t)rect->width - x_padding * 2;

    if (available <= 0)
    {
        return;
    }

    size_t maximum = (size_t)available / UI_GLYPH_WIDTH;

    if (maximum == 0)
    {
        return;
    }

    size_t length = text_length(text);

    if (length <= maximum)
    {
        ui_draw_text(
            text,
            rect->x + x_padding,
            rect->y +
                (int32_t)((rect->height - UI_GLYPH_HEIGHT) / 2U),
            color
        );

        return;
    }

    char buffer[128];

    if (maximum >= sizeof(buffer))
    {
        maximum = sizeof(buffer) - 1U;
    }

    size_t copy_count = maximum;

    if (copy_count >= 3U)
    {
        copy_count -= 3U;
    }

    for (size_t index = 0; index < copy_count; index++)
    {
        buffer[index] = text[index];
    }

    size_t position = copy_count;

    if (maximum >= 3U)
    {
        buffer[position++] = '.';
        buffer[position++] = '.';
        buffer[position++] = '.';
    }

    buffer[position] = '\0';

    ui_draw_text(
        buffer,
        rect->x + x_padding,
        rect->y +
            (int32_t)((rect->height - UI_GLYPH_HEIGHT) / 2U),
        color
    );
}

void ui_draw_horizontal_line(
    int32_t x,
    int32_t y,
    uint32_t width,
    uint32_t thickness,
    uint32_t color
)
{
    ui_rect_t rectangle = {
        .x = x,
        .y = y,
        .width = width,
        .height = thickness
    };

    ui_fill_rect(&rectangle, color);
}

void ui_draw_vertical_line(
    int32_t x,
    int32_t y,
    uint32_t height,
    uint32_t thickness,
    uint32_t color
)
{
    ui_rect_t rectangle = {
        .x = x,
        .y = y,
        .width = thickness,
        .height = height
    };

    ui_fill_rect(&rectangle, color);
}

void ui_draw_cursor(
    int32_t x,
    int32_t y
)
{
    static const uint16_t rows[16] = {
        0x8000, 0xC000, 0xE000, 0xF000,
        0xF800, 0xFC00, 0xFE00, 0xFF00,
        0xFF80, 0xF000, 0xD800, 0xCC00,
        0x8600, 0x0300, 0x0180, 0x0000
    };

    for (uint32_t row = 0; row < 16; row++)
    {
        for (uint32_t column = 0; column < 16; column++)
        {
            if ((rows[row] & (1U << (15U - column))) == 0)
            {
                continue;
            }

            int32_t pixel_x = x + (int32_t)column;
            int32_t pixel_y = y + (int32_t)row;

            if (
                pixel_x >= 0 &&
                pixel_y >= 0 &&
                pixel_x < (int32_t)graphics_width() &&
                pixel_y < (int32_t)graphics_height()
            )
            {
                draw_pixel(
                    (uint32_t)pixel_x,
                    (uint32_t)pixel_y,
                    column == 0 || row == 0 ?
                        0xFFFFFFU : 0x101820U
                );
            }
        }
    }
}
