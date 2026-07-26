#include "graphics.h"
#include "font.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The software backbuffer is the universal GUI rendering surface.
 * QEMU/Limine resolutions used by LatterOS fit within this limit.
 * Larger framebuffers fall back to direct rendering automatically.
 */
#define GRAPHICS_MAX_WIDTH  1920U
#define GRAPHICS_MAX_HEIGHT 1200U
#define GRAPHICS_MAX_PIXELS \
    ((uint64_t)GRAPHICS_MAX_WIDTH * GRAPHICS_MAX_HEIGHT)

static uint32_t *front_buffer;
static uint64_t front_pitch;
static uint32_t screen_width;
static uint32_t screen_height;

static uint32_t back_buffer[GRAPHICS_MAX_PIXELS]
    __attribute__((aligned(64)));

static uint32_t *draw_buffer;
static bool buffered;
static bool deferred_mode;

static int32_t clip_x;
static int32_t clip_y;
static int32_t clip_right;
static int32_t clip_bottom;

static uint64_t front_stride(void)
{
    return front_pitch / sizeof(uint32_t);
}

static uint64_t draw_stride(void)
{
    if (buffered)
    {
        return screen_width;
    }

    return front_stride();
}

static bool clip_rectangle(
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height,
    uint32_t *out_x,
    uint32_t *out_y,
    uint32_t *out_width,
    uint32_t *out_height
)
{
    if (
        width == 0 ||
        height == 0 ||
        x >= screen_width ||
        y >= screen_height
    )
    {
        return false;
    }

    uint64_t right = (uint64_t)x + width;
    uint64_t bottom = (uint64_t)y + height;

    if (right > screen_width)
    {
        right = screen_width;
    }

    if (bottom > screen_height)
    {
        bottom = screen_height;
    }

    uint32_t clipped_x = x;
    uint32_t clipped_y = y;

    if ((int32_t)clipped_x < clip_x)
    {
        clipped_x = (uint32_t)clip_x;
    }

    if ((int32_t)clipped_y < clip_y)
    {
        clipped_y = (uint32_t)clip_y;
    }

    if ((int32_t)right > clip_right)
    {
        right = (uint32_t)clip_right;
    }

    if ((int32_t)bottom > clip_bottom)
    {
        bottom = (uint32_t)clip_bottom;
    }

    if (
        right <= clipped_x ||
        bottom <= clipped_y
    )
    {
        return false;
    }

    *out_x = clipped_x;
    *out_y = clipped_y;
    *out_width = (uint32_t)right - clipped_x;
    *out_height = (uint32_t)bottom - clipped_y;

    return true;
}

void graphics_init(
    uint32_t *framebuffer,
    uint64_t pitch,
    uint64_t width,
    uint64_t height
)
{
    front_buffer = framebuffer;
    front_pitch = pitch;
    screen_width = (uint32_t)width;
    screen_height = (uint32_t)height;

    buffered =
        framebuffer != NULL &&
        width <= GRAPHICS_MAX_WIDTH &&
        height <= GRAPHICS_MAX_HEIGHT &&
        width * height <= GRAPHICS_MAX_PIXELS;

    draw_buffer = buffered ?
        back_buffer :
        front_buffer;

    deferred_mode = false;
    graphics_reset_clip();
}

void graphics_set_deferred(bool deferred)
{
    deferred_mode = deferred && buffered;
}

bool graphics_is_deferred(void)
{
    return deferred_mode;
}

uint32_t graphics_width(void)
{
    return screen_width;
}

uint32_t graphics_height(void)
{
    return screen_height;
}

void graphics_set_clip(
    int32_t x,
    int32_t y,
    uint32_t width,
    uint32_t height
)
{
    int64_t right = (int64_t)x + width;
    int64_t bottom = (int64_t)y + height;

    if (x < 0)
    {
        x = 0;
    }

    if (y < 0)
    {
        y = 0;
    }

    if (right > screen_width)
    {
        right = screen_width;
    }

    if (bottom > screen_height)
    {
        bottom = screen_height;
    }

    clip_x = x;
    clip_y = y;
    clip_right = (int32_t)right;
    clip_bottom = (int32_t)bottom;

    if (clip_right < clip_x)
    {
        clip_right = clip_x;
    }

    if (clip_bottom < clip_y)
    {
        clip_bottom = clip_y;
    }
}

void graphics_reset_clip(void)
{
    clip_x = 0;
    clip_y = 0;
    clip_right = (int32_t)screen_width;
    clip_bottom = (int32_t)screen_height;
}

void graphics_get_clip(
    int32_t *x,
    int32_t *y,
    int32_t *right,
    int32_t *bottom
)
{
    if (x != NULL)
    {
        *x = clip_x;
    }

    if (y != NULL)
    {
        *y = clip_y;
    }

    if (right != NULL)
    {
        *right = clip_right;
    }

    if (bottom != NULL)
    {
        *bottom = clip_bottom;
    }
}

uint32_t graphics_get_pixel(
    uint32_t x,
    uint32_t y
)
{
    if (
        draw_buffer == NULL ||
        x >= screen_width ||
        y >= screen_height
    )
    {
        return 0;
    }

    return draw_buffer[
        (uint64_t)y * draw_stride() + x
    ];
}

void graphics_present_rectangle(
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height
)
{
    if (
        !buffered ||
        front_buffer == NULL ||
        width == 0 ||
        height == 0 ||
        x >= screen_width ||
        y >= screen_height
    )
    {
        return;
    }

    uint64_t right = (uint64_t)x + width;
    uint64_t bottom = (uint64_t)y + height;

    if (right > screen_width)
    {
        right = screen_width;
    }

    if (bottom > screen_height)
    {
        bottom = screen_height;
    }

    uint64_t destination_stride = front_stride();

    for (uint64_t row = y; row < bottom; row++)
    {
        const uint32_t *source =
            &back_buffer[row * screen_width + x];

        uint32_t *destination =
            &front_buffer[row * destination_stride + x];

        for (
            uint64_t column = x;
            column < right;
            column++
        )
        {
            *destination = *source;
            destination++;
            source++;
        }
    }
}

void graphics_present(void)
{
    graphics_present_rectangle(
        0,
        0,
        screen_width,
        screen_height
    );
}

void draw_pixel(
    uint32_t x,
    uint32_t y,
    uint32_t color
)
{
    if (
        draw_buffer == NULL ||
        x >= screen_width ||
        y >= screen_height ||
        (int32_t)x < clip_x ||
        (int32_t)y < clip_y ||
        (int32_t)x >= clip_right ||
        (int32_t)y >= clip_bottom
    )
    {
        return;
    }

    draw_buffer[
        (uint64_t)y * draw_stride() + x
    ] = color;

    if (
        buffered &&
        !deferred_mode &&
        front_buffer != NULL
    )
    {
        front_buffer[
            (uint64_t)y * front_stride() + x
        ] = color;
    }
}

void draw_rectangle(
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height,
    uint32_t color
)
{
    uint32_t clipped_x;
    uint32_t clipped_y;
    uint32_t clipped_width;
    uint32_t clipped_height;

    if (
        draw_buffer == NULL ||
        !clip_rectangle(
            x,
            y,
            width,
            height,
            &clipped_x,
            &clipped_y,
            &clipped_width,
            &clipped_height
        )
    )
    {
        return;
    }

    uint64_t stride = draw_stride();
    uint64_t end_x =
        (uint64_t)clipped_x + clipped_width;

    uint64_t end_y =
        (uint64_t)clipped_y + clipped_height;

    for (
        uint64_t row = clipped_y;
        row < end_y;
        row++
    )
    {
        uint32_t *destination =
            &draw_buffer[row * stride + clipped_x];

        for (
            uint64_t column = clipped_x;
            column < end_x;
            column++
        )
        {
            *destination = color;
            destination++;
        }
    }

    if (buffered && !deferred_mode)
    {
        graphics_present_rectangle(
            clipped_x,
            clipped_y,
            clipped_width,
            clipped_height
        );
    }
}

void graphics_clear(uint32_t color)
{
    draw_rectangle(
        0,
        0,
        screen_width,
        screen_height,
        color
    );
}

void draw_character(
    char character,
    uint32_t x,
    uint32_t y,
    uint32_t color
)
{
    const uint8_t *bitmap =
        font_get_character(character);

    if (bitmap == NULL)
    {
        return;
    }

    for (uint32_t row = 0; row < 8; row++)
    {
        uint8_t bits = bitmap[row];

        if (bits == 0)
        {
            continue;
        }

        for (
            uint32_t column = 0;
            column < 8;
            column++
        )
        {
            if (bits & (uint8_t)(0x80U >> column))
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
    if (text == NULL)
    {
        return;
    }

    uint32_t current_x = x;

    for (
        uint32_t index = 0;
        text[index] != '\0';
        index++
    )
    {
        draw_character(
            text[index],
            current_x,
            y,
            color
        );

        current_x += 8;
    }
}
