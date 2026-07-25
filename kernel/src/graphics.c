#include "graphics.h"
#include "font.h"

static uint32_t *screen;
static uint64_t screen_pitch;
static uint64_t screen_width;
static uint64_t screen_height;

void graphics_init(
    uint32_t *framebuffer,
    uint64_t pitch,
    uint64_t width,
    uint64_t height
)
{
    screen = framebuffer;
    screen_pitch = pitch;
    screen_width = width;
    screen_height = height;
}

void graphics_clear(uint32_t color)
{
    draw_rectangle(
        0,
        0,
        (uint32_t)screen_width,
        (uint32_t)screen_height,
        color
    );
}

void draw_pixel(
    uint32_t x,
    uint32_t y,
    uint32_t color
)
{
    if (x >= screen_width || y >= screen_height)
    {
        return;
    }

    screen[y * (screen_pitch / 4) + x] = color;
}

void draw_rectangle(
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height,
    uint32_t color
)
{
    for (uint32_t row = 0; row < height; row++)
    {
        for (uint32_t column = 0; column < width; column++)
        {
            draw_pixel(
                x + column,
                y + row,
                color
            );
        }
    }
}

void draw_character(
    char character,
    uint32_t x,
    uint32_t y,
    uint32_t color
)
{
    const uint8_t *bitmap = font_get_character(character);

    if (bitmap == 0)
    {
        return;
    }

    for (uint32_t row = 0; row < 8; row++)
    {
        for (uint32_t column = 0; column < 8; column++)
        {
            if (bitmap[row] & (0b10000000 >> column))
            {
                draw_pixel(
                    x + column,
                    y + row,
                    color
                );
            }
        }
    }
}

void draw_text(
    const char *text,
    uint32_t x,
    uint32_t y,
    uint32_t color
)
{
    uint32_t current_x = x;

    for (uint32_t i = 0; text[i] != '\0'; i++)
    {
        draw_character(
            text[i],
            current_x,
            y,
            color
        );

        current_x += 8;
    }
}