#include "surface.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static uint32_t minimum_u32(uint32_t first, uint32_t second)
{
    return first < second ? first : second;
}

static bool clip_rect_to_surface(
    const surface_t *surface,
    const ui_rect_t *input,
    ui_rect_t *output
)
{
    if (
        !surface_valid(surface) ||
        input == NULL ||
        output == NULL ||
        input->width == 0 ||
        input->height == 0
    )
    {
        return false;
    }

    int64_t left = input->x;
    int64_t top = input->y;
    int64_t right = left + input->width;
    int64_t bottom = top + input->height;

    if (
        right <= 0 ||
        bottom <= 0 ||
        left >= surface->width ||
        top >= surface->height
    )
    {
        return false;
    }

    if (left < 0)
    {
        left = 0;
    }

    if (top < 0)
    {
        top = 0;
    }

    if (right > surface->width)
    {
        right = surface->width;
    }

    if (bottom > surface->height)
    {
        bottom = surface->height;
    }

    output->x = (int32_t)left;
    output->y = (int32_t)top;
    output->width = (uint32_t)(right - left);
    output->height = (uint32_t)(bottom - top);
    return output->width != 0 && output->height != 0;
}

static void copy_pixels(
    uint32_t *destination,
    const uint32_t *source,
    uint32_t count
)
{
    while (count >= 8U)
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
        count -= 8U;
    }

    while (count != 0U)
    {
        *destination++ = *source++;
        count--;
    }
}

static uint32_t blend_pixel(
    uint32_t destination,
    uint32_t source,
    uint8_t opacity
)
{
    uint32_t source_alpha = (source >> 24) & 0xFFU;
    source_alpha = source_alpha * opacity / 255U;

    if (source_alpha == 0U)
    {
        return destination;
    }

    if (source_alpha >= 255U)
    {
        return source | 0xFF000000U;
    }

    uint32_t inverse = 255U - source_alpha;
    uint32_t source_red = (source >> 16) & 0xFFU;
    uint32_t source_green = (source >> 8) & 0xFFU;
    uint32_t source_blue = source & 0xFFU;
    uint32_t destination_red = (destination >> 16) & 0xFFU;
    uint32_t destination_green = (destination >> 8) & 0xFFU;
    uint32_t destination_blue = destination & 0xFFU;

    uint32_t red =
        (source_red * source_alpha + destination_red * inverse + 127U) /
        255U;

    uint32_t green =
        (source_green * source_alpha + destination_green * inverse + 127U) /
        255U;

    uint32_t blue =
        (source_blue * source_alpha + destination_blue * inverse + 127U) /
        255U;

    return 0xFF000000U | (red << 16) | (green << 8) | blue;
}

bool surface_init(
    surface_t *surface,
    uint32_t *pixels,
    uint32_t width,
    uint32_t height,
    uint32_t stride
)
{
    if (
        surface == NULL ||
        pixels == NULL ||
        width == 0U ||
        height == 0U ||
        stride < width
    )
    {
        return false;
    }

    surface->pixels = pixels;
    surface->width = width;
    surface->height = height;
    surface->stride = stride;
    return true;
}

bool surface_valid(const surface_t *surface)
{
    return
        surface != NULL &&
        surface->pixels != NULL &&
        surface->width != 0U &&
        surface->height != 0U &&
        surface->stride >= surface->width;
}

void surface_clear(surface_t *surface, uint32_t color)
{
    if (!surface_valid(surface))
    {
        return;
    }

    for (uint32_t y = 0; y < surface->height; y++)
    {
        uint32_t *row = &surface->pixels[(uint64_t)y * surface->stride];

        for (uint32_t x = 0; x < surface->width; x++)
        {
            row[x] = color;
        }
    }
}

void surface_fill_rect(
    surface_t *surface,
    const ui_rect_t *rectangle,
    uint32_t color
)
{
    ui_rect_t clipped;

    if (!clip_rect_to_surface(surface, rectangle, &clipped))
    {
        return;
    }

    for (uint32_t row = 0; row < clipped.height; row++)
    {
        uint32_t *destination =
            &surface->pixels[
                (uint64_t)(clipped.y + (int32_t)row) * surface->stride +
                (uint32_t)clipped.x
            ];

        for (uint32_t column = 0; column < clipped.width; column++)
        {
            destination[column] = color;
        }
    }
}

bool surface_copy_rect(
    surface_t *destination,
    int32_t destination_x,
    int32_t destination_y,
    const surface_t *source,
    const ui_rect_t *source_rectangle
)
{
    if (
        !surface_valid(destination) ||
        !surface_valid(source) ||
        source_rectangle == NULL ||
        source_rectangle->width == 0U ||
        source_rectangle->height == 0U
    )
    {
        return false;
    }

    int32_t source_x = source_rectangle->x;
    int32_t source_y = source_rectangle->y;
    int32_t width = (int32_t)source_rectangle->width;
    int32_t height = (int32_t)source_rectangle->height;

    if (source_x < 0)
    {
        destination_x -= source_x;
        width += source_x;
        source_x = 0;
    }

    if (source_y < 0)
    {
        destination_y -= source_y;
        height += source_y;
        source_y = 0;
    }

    if (destination_x < 0)
    {
        source_x -= destination_x;
        width += destination_x;
        destination_x = 0;
    }

    if (destination_y < 0)
    {
        source_y -= destination_y;
        height += destination_y;
        destination_y = 0;
    }

    width = (int32_t)minimum_u32(
        (uint32_t)(width > 0 ? width : 0),
        source->width > (uint32_t)source_x ?
            source->width - (uint32_t)source_x : 0U
    );

    height = (int32_t)minimum_u32(
        (uint32_t)(height > 0 ? height : 0),
        source->height > (uint32_t)source_y ?
            source->height - (uint32_t)source_y : 0U
    );

    width = (int32_t)minimum_u32(
        (uint32_t)(width > 0 ? width : 0),
        destination->width > (uint32_t)destination_x ?
            destination->width - (uint32_t)destination_x : 0U
    );

    height = (int32_t)minimum_u32(
        (uint32_t)(height > 0 ? height : 0),
        destination->height > (uint32_t)destination_y ?
            destination->height - (uint32_t)destination_y : 0U
    );

    if (width <= 0 || height <= 0)
    {
        return false;
    }

    for (int32_t row = 0; row < height; row++)
    {
        copy_pixels(
            &destination->pixels[
                (uint64_t)(destination_y + row) * destination->stride +
                (uint32_t)destination_x
            ],
            &source->pixels[
                (uint64_t)(source_y + row) * source->stride +
                (uint32_t)source_x
            ],
            (uint32_t)width
        );
    }

    return true;
}

bool surface_alpha_blit(
    surface_t *destination,
    int32_t destination_x,
    int32_t destination_y,
    const surface_t *source,
    const ui_rect_t *source_rectangle,
    uint8_t opacity
)
{
    if (
        !surface_valid(destination) ||
        !surface_valid(source) ||
        source_rectangle == NULL ||
        opacity == 0U
    )
    {
        return false;
    }

    int32_t source_x = source_rectangle->x;
    int32_t source_y = source_rectangle->y;
    int32_t width = (int32_t)source_rectangle->width;
    int32_t height = (int32_t)source_rectangle->height;

    if (source_x < 0)
    {
        destination_x -= source_x;
        width += source_x;
        source_x = 0;
    }

    if (source_y < 0)
    {
        destination_y -= source_y;
        height += source_y;
        source_y = 0;
    }

    if (destination_x < 0)
    {
        source_x -= destination_x;
        width += destination_x;
        destination_x = 0;
    }

    if (destination_y < 0)
    {
        source_y -= destination_y;
        height += destination_y;
        destination_y = 0;
    }

    if (
        width <= 0 ||
        height <= 0 ||
        (uint32_t)source_x >= source->width ||
        (uint32_t)source_y >= source->height ||
        (uint32_t)destination_x >= destination->width ||
        (uint32_t)destination_y >= destination->height
    )
    {
        return false;
    }

    uint32_t clipped_width = minimum_u32(
        (uint32_t)width,
        source->width - (uint32_t)source_x
    );

    clipped_width = minimum_u32(
        clipped_width,
        destination->width - (uint32_t)destination_x
    );

    uint32_t clipped_height = minimum_u32(
        (uint32_t)height,
        source->height - (uint32_t)source_y
    );

    clipped_height = minimum_u32(
        clipped_height,
        destination->height - (uint32_t)destination_y
    );

    for (uint32_t row = 0; row < clipped_height; row++)
    {
        uint32_t *destination_row =
            &destination->pixels[
                (uint64_t)(destination_y + (int32_t)row) *
                    destination->stride +
                (uint32_t)destination_x
            ];

        const uint32_t *source_row =
            &source->pixels[
                (uint64_t)(source_y + (int32_t)row) * source->stride +
                (uint32_t)source_x
            ];

        for (uint32_t column = 0; column < clipped_width; column++)
        {
            destination_row[column] = blend_pixel(
                destination_row[column],
                source_row[column],
                opacity
            );
        }
    }

    return true;
}

bool surface_scale_nearest(
    surface_t *destination,
    const ui_rect_t *destination_rectangle,
    const surface_t *source,
    const ui_rect_t *source_rectangle
)
{
    if (
        !surface_valid(destination) ||
        !surface_valid(source) ||
        destination_rectangle == NULL ||
        source_rectangle == NULL ||
        destination_rectangle->width == 0U ||
        destination_rectangle->height == 0U ||
        source_rectangle->width == 0U ||
        source_rectangle->height == 0U
    )
    {
        return false;
    }

    ui_rect_t clipped_destination;

    if (!clip_rect_to_surface(
        destination,
        destination_rectangle,
        &clipped_destination
    ))
    {
        return false;
    }

    for (uint32_t y = 0; y < clipped_destination.height; y++)
    {
        uint32_t destination_relative_y =
            (uint32_t)(clipped_destination.y - destination_rectangle->y) + y;

        uint64_t source_relative_y =
            (uint64_t)destination_relative_y *
            source_rectangle->height /
            destination_rectangle->height;

        uint32_t source_y =
            (uint32_t)source_rectangle->y + (uint32_t)source_relative_y;

        if (source_y >= source->height)
        {
            continue;
        }

        for (uint32_t x = 0; x < clipped_destination.width; x++)
        {
            uint32_t destination_relative_x =
                (uint32_t)(clipped_destination.x - destination_rectangle->x) + x;

            uint64_t source_relative_x =
                (uint64_t)destination_relative_x *
                source_rectangle->width /
                destination_rectangle->width;

            uint32_t source_x =
                (uint32_t)source_rectangle->x +
                (uint32_t)source_relative_x;

            if (source_x >= source->width)
            {
                continue;
            }

            destination->pixels[
                (uint64_t)(clipped_destination.y + (int32_t)y) *
                    destination->stride +
                (uint32_t)clipped_destination.x + x
            ] = source->pixels[
                (uint64_t)source_y * source->stride + source_x
            ];
        }
    }

    return true;
}

uint64_t surface_checksum(const surface_t *surface)
{
    if (!surface_valid(surface))
    {
        return 0;
    }

    uint64_t checksum = 1469598103934665603ULL;

    for (uint32_t y = 0; y < surface->height; y++)
    {
        const uint32_t *row =
            &surface->pixels[(uint64_t)y * surface->stride];

        for (uint32_t x = 0; x < surface->width; x++)
        {
            uint32_t value = row[x];

            for (uint32_t byte = 0; byte < 4U; byte++)
            {
                checksum ^= (value >> (byte * 8U)) & 0xFFU;
                checksum *= 1099511628211ULL;
            }
        }
    }

    return checksum;
}
