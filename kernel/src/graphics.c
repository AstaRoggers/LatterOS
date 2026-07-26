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

static bool cursor_overlay_visible;
static int32_t cursor_overlay_x;
static int32_t cursor_overlay_y;
static uint64_t cursor_updates;
static uint64_t surface_blits;

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


static void copy_pixels_unrolled(
    uint32_t *destination,
    const uint32_t *source,
    uint64_t count
)
{
    while (count >= 8)
    {
        destination[0] = source[0];
        destination[1] = source[1];
        destination[2] = source[2];
        destination[3] = source[3];
        destination[4] = source[4];
        destination[5] = source[5];
        destination[6] = source[6];
        destination[7] = source[7];

        destination += 8;
        source += 8;
        count -= 8;
    }

    while (count != 0)
    {
        *destination = *source;
        destination++;
        source++;
        count--;
    }
}

static void fill_pixels_unrolled(
    uint32_t *destination,
    uint32_t color,
    uint64_t count
)
{
    while (count >= 8)
    {
        destination[0] = color;
        destination[1] = color;
        destination[2] = color;
        destination[3] = color;
        destination[4] = color;
        destination[5] = color;
        destination[6] = color;
        destination[7] = color;

        destination += 8;
        count -= 8;
    }

    while (count != 0)
    {
        *destination = color;
        destination++;
        count--;
    }
}

static void draw_cursor_mask_to_front(
    const uint16_t *rows,
    uint32_t color
)
{
    if (
        rows == NULL ||
        front_buffer == NULL
    )
    {
        return;
    }

    uint64_t stride = front_stride();

    for (uint32_t row = 0; row < 16; row++)
    {
        int32_t target_y =
            cursor_overlay_y + (int32_t)row;

        if (
            target_y < 0 ||
            target_y >= (int32_t)screen_height
        )
        {
            continue;
        }

        for (uint32_t column = 0; column < 16; column++)
        {
            if (!(rows[row] & (uint16_t)(0x8000U >> column)))
            {
                continue;
            }

            int32_t target_x =
                cursor_overlay_x + (int32_t)column;

            if (
                target_x < 0 ||
                target_x >= (int32_t)screen_width
            )
            {
                continue;
            }

            front_buffer[
                (uint64_t)target_y * stride +
                (uint32_t)target_x
            ] = color;
        }
    }
}

static void draw_cursor_to_front(void)
{
    /*
     * A black outline and white center keep the pointer visible over both
     * dark desktop areas and bright window controls. The previous mostly
     * white cursor could appear to vanish over terminal fields and borders.
     */
    static const uint16_t outline_rows[16] = {
        0x8000, 0xC000, 0xE000, 0xF000,
        0xF800, 0xFC00, 0xFE00, 0xFF00,
        0xFF80, 0xFFC0, 0xFEC0, 0xCE60,
        0x8E60, 0x0630, 0x0630, 0x0000
    };

    static const uint16_t fill_rows[16] = {
        0x0000, 0x4000, 0x6000, 0x7000,
        0x7800, 0x7C00, 0x7E00, 0x7F00,
        0x7F00, 0x7C00, 0x6C00, 0x4400,
        0x0400, 0x0200, 0x0200, 0x0000
    };

    if (
        !cursor_overlay_visible ||
        front_buffer == NULL
    )
    {
        return;
    }

    draw_cursor_mask_to_front(
        outline_rows,
        0x000000
    );

    draw_cursor_mask_to_front(
        fill_rows,
        0xFFFFFF
    );
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
    cursor_overlay_visible = false;
    cursor_overlay_x = 0;
    cursor_overlay_y = 0;
    graphics_reset_motion_statistics();
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

static void present_rectangle_raw(
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

        copy_pixels_unrolled(
            destination,
            source,
            right - x
        );
    }
}

static bool rectangle_overlaps_cursor(
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height
)
{
    if (!cursor_overlay_visible)
    {
        return false;
    }

    int64_t right = (int64_t)x + width;
    int64_t bottom = (int64_t)y + height;
    int64_t cursor_right = cursor_overlay_x + 16;
    int64_t cursor_bottom = cursor_overlay_y + 16;

    return
        (int64_t)x < cursor_right &&
        right > cursor_overlay_x &&
        (int64_t)y < cursor_bottom &&
        bottom > cursor_overlay_y;
}

void graphics_present_rectangle(
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height
)
{
    present_rectangle_raw(
        x,
        y,
        width,
        height
    );

    /*
     * Any direct framebuffer presentation can cover the software cursor.
     * Reapply it immediately when the updated rectangle intersects it.
     */
    if (rectangle_overlaps_cursor(x, y, width, height))
    {
        draw_cursor_to_front();
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


bool graphics_capture_rectangle(
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height,
    uint32_t *destination,
    uint32_t destination_stride
)
{
    if (
        draw_buffer == NULL ||
        destination == NULL ||
        width == 0 ||
        height == 0 ||
        destination_stride < width ||
        x >= screen_width ||
        y >= screen_height ||
        (uint64_t)x + width > screen_width ||
        (uint64_t)y + height > screen_height
    )
    {
        return false;
    }

    uint64_t stride = draw_stride();

    for (uint32_t row = 0; row < height; row++)
    {
        copy_pixels_unrolled(
            &destination[(uint64_t)row * destination_stride],
            &draw_buffer[(uint64_t)(y + row) * stride + x],
            width
        );
    }

    return true;
}

void graphics_blit_surface(
    const uint32_t *source,
    uint32_t source_stride,
    uint32_t source_width,
    uint32_t source_height,
    int32_t destination_x,
    int32_t destination_y
)
{
    if (
        source == NULL ||
        draw_buffer == NULL ||
        source_width == 0 ||
        source_height == 0 ||
        source_stride < source_width
    )
    {
        return;
    }

    int32_t left = destination_x;
    int32_t top = destination_y;
    int32_t right =
        destination_x + (int32_t)source_width;
    int32_t bottom =
        destination_y + (int32_t)source_height;

    if (left < clip_x)
    {
        left = clip_x;
    }

    if (top < clip_y)
    {
        top = clip_y;
    }

    if (right > clip_right)
    {
        right = clip_right;
    }

    if (bottom > clip_bottom)
    {
        bottom = clip_bottom;
    }

    if (left < 0)
    {
        left = 0;
    }

    if (top < 0)
    {
        top = 0;
    }

    if (right > (int32_t)screen_width)
    {
        right = (int32_t)screen_width;
    }

    if (bottom > (int32_t)screen_height)
    {
        bottom = (int32_t)screen_height;
    }

    if (right <= left || bottom <= top)
    {
        return;
    }

    uint32_t source_x =
        (uint32_t)(left - destination_x);
    uint32_t source_y =
        (uint32_t)(top - destination_y);
    uint32_t copy_width =
        (uint32_t)(right - left);
    uint32_t copy_height =
        (uint32_t)(bottom - top);
    uint64_t stride = draw_stride();

    for (uint32_t row = 0; row < copy_height; row++)
    {
        copy_pixels_unrolled(
            &draw_buffer[
                (uint64_t)(top + (int32_t)row) * stride +
                (uint32_t)left
            ],
            &source[
                (uint64_t)(source_y + row) * source_stride +
                source_x
            ],
            copy_width
        );
    }

    surface_blits++;
}

void graphics_cursor_show(int32_t x, int32_t y)
{
    if (!buffered || front_buffer == NULL)
    {
        return;
    }

    if (cursor_overlay_visible)
    {
        present_rectangle_raw(
            (uint32_t)cursor_overlay_x,
            (uint32_t)cursor_overlay_y,
            16,
            16
        );
    }

    cursor_overlay_x = x;
    cursor_overlay_y = y;
    cursor_overlay_visible = true;
    cursor_updates++;
    draw_cursor_to_front();
}

void graphics_cursor_move(int32_t x, int32_t y)
{
    graphics_cursor_show(x, y);
}

void graphics_cursor_hide(void)
{
    if (!cursor_overlay_visible)
    {
        return;
    }

    int32_t old_x = cursor_overlay_x;
    int32_t old_y = cursor_overlay_y;
    cursor_overlay_visible = false;

    if (old_x >= 0 && old_y >= 0)
    {
        present_rectangle_raw(
            (uint32_t)old_x,
            (uint32_t)old_y,
            16,
            16
        );
    }
}

void graphics_cursor_refresh(void)
{
    draw_cursor_to_front();
}

uint64_t graphics_cursor_update_count(void)
{
    return cursor_updates;
}

uint64_t graphics_surface_blit_count(void)
{
    return surface_blits;
}

void graphics_reset_motion_statistics(void)
{
    cursor_updates = 0;
    surface_blits = 0;
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

        fill_pixels_unrolled(
            destination,
            color,
            end_x - clipped_x
        );
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
    if (
        x >= screen_width ||
        y >= screen_height ||
        (int64_t)x + 8 <= clip_x ||
        (int64_t)y + 8 <= clip_y ||
        (int32_t)x >= clip_right ||
        (int32_t)y >= clip_bottom
    )
    {
        return;
    }

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
