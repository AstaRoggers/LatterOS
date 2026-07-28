#include "visual_effects.h"

#include "graphics.h"
#include "ui.h"

#include <stddef.h>
#include <stdint.h>

static uint32_t blend_color(
    uint32_t destination,
    uint32_t source,
    uint8_t opacity
)
{
    if (opacity == 0U)
    {
        return destination;
    }

    if (opacity == 255U)
    {
        return source & 0x00FFFFFFU;
    }

    uint32_t inverse = 255U - opacity;
    uint32_t source_red = (source >> 16) & 0xFFU;
    uint32_t source_green = (source >> 8) & 0xFFU;
    uint32_t source_blue = source & 0xFFU;
    uint32_t destination_red = (destination >> 16) & 0xFFU;
    uint32_t destination_green = (destination >> 8) & 0xFFU;
    uint32_t destination_blue = destination & 0xFFU;

    uint32_t red =
        (source_red * opacity + destination_red * inverse + 127U) /
        255U;

    uint32_t green =
        (source_green * opacity + destination_green * inverse + 127U) /
        255U;

    uint32_t blue =
        (source_blue * opacity + destination_blue * inverse + 127U) /
        255U;

    return (red << 16) | (green << 8) | blue;
}

static bool clipped_bounds(
    const ui_rect_t *rectangle,
    int32_t *left,
    int32_t *top,
    int32_t *right,
    int32_t *bottom
)
{
    if (
        rectangle == NULL ||
        left == NULL ||
        top == NULL ||
        right == NULL ||
        bottom == NULL ||
        rectangle->width == 0U ||
        rectangle->height == 0U
    )
    {
        return false;
    }

    int64_t candidate_left = rectangle->x;
    int64_t candidate_top = rectangle->y;
    int64_t candidate_right = candidate_left + rectangle->width;
    int64_t candidate_bottom = candidate_top + rectangle->height;

    if (
        candidate_right <= 0 ||
        candidate_bottom <= 0 ||
        candidate_left >= graphics_width() ||
        candidate_top >= graphics_height()
    )
    {
        return false;
    }

    if (candidate_left < 0)
    {
        candidate_left = 0;
    }

    if (candidate_top < 0)
    {
        candidate_top = 0;
    }

    if (candidate_right > graphics_width())
    {
        candidate_right = graphics_width();
    }

    if (candidate_bottom > graphics_height())
    {
        candidate_bottom = graphics_height();
    }

    *left = (int32_t)candidate_left;
    *top = (int32_t)candidate_top;
    *right = (int32_t)candidate_right;
    *bottom = (int32_t)candidate_bottom;
    return *right > *left && *bottom > *top;
}

void visual_effects_blend_rect(
    const ui_rect_t *rectangle,
    uint32_t color,
    uint8_t opacity
)
{
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;

    if (
        opacity == 0U ||
        !clipped_bounds(
            rectangle,
            &left,
            &top,
            &right,
            &bottom
        )
    )
    {
        return;
    }

    for (int32_t y = top; y < bottom; y++)
    {
        for (int32_t x = left; x < right; x++)
        {
            uint32_t destination = graphics_get_pixel(
                (uint32_t)x,
                (uint32_t)y
            );

            draw_pixel(
                (uint32_t)x,
                (uint32_t)y,
                blend_color(destination, color, opacity)
            );
        }
    }
}

void visual_effects_draw_shadow(
    const ui_rect_t *bounds,
    uint32_t color
)
{
    if (
        bounds == NULL ||
        bounds->width == 0U ||
        bounds->height == 0U
    )
    {
        return;
    }

    static const uint8_t opacity[5] = {
        18U, 24U, 32U, 42U, 54U
    };

    for (uint32_t layer = 0; layer < 5U; layer++)
    {
        int32_t spread = (int32_t)(5U - layer);
        int32_t offset = (int32_t)(layer + 2U);

        ui_rect_t right_band = {
            .x = bounds->x + (int32_t)bounds->width + offset - 1,
            .y = bounds->y + spread,
            .width = (uint32_t)(spread + 2),
            .height = bounds->height
        };

        ui_rect_t bottom_band = {
            .x = bounds->x + spread,
            .y = bounds->y + (int32_t)bounds->height + offset - 1,
            .width = bounds->width,
            .height = (uint32_t)(spread + 2)
        };

        visual_effects_blend_rect(
            &right_band,
            color,
            opacity[layer]
        );

        visual_effects_blend_rect(
            &bottom_band,
            color,
            opacity[layer]
        );
    }
}

static uint32_t rounded_inset_for_row(
    uint32_t radius,
    uint32_t row
)
{
    if (radius == 0U || row >= radius)
    {
        return 0U;
    }

    int32_t center = (int32_t)radius - 1;
    int32_t dy = center - (int32_t)row;
    int32_t limit = (int32_t)(radius * radius);
    int32_t x = 0;

    while (
        x < center &&
        (center - x) * (center - x) + dy * dy > limit
    )
    {
        x++;
    }

    return (uint32_t)x;
}

void visual_effects_fill_rounded_rect(
    const ui_rect_t *bounds,
    uint32_t radius,
    uint32_t color
)
{
    if (
        bounds == NULL ||
        bounds->width == 0U ||
        bounds->height == 0U
    )
    {
        return;
    }

    uint32_t maximum_radius = bounds->width / 2U;

    if (bounds->height / 2U < maximum_radius)
    {
        maximum_radius = bounds->height / 2U;
    }

    if (radius > maximum_radius)
    {
        radius = maximum_radius;
    }

    if (radius == 0U)
    {
        ui_fill_rect(bounds, color);
        return;
    }

    ui_rect_t middle = {
        .x = bounds->x,
        .y = bounds->y + (int32_t)radius,
        .width = bounds->width,
        .height = bounds->height - radius * 2U
    };

    ui_fill_rect(&middle, color);

    for (uint32_t row = 0; row < radius; row++)
    {
        uint32_t inset = rounded_inset_for_row(radius, row);
        uint32_t width = bounds->width > inset * 2U ?
            bounds->width - inset * 2U : 0U;

        if (width == 0U)
        {
            continue;
        }

        ui_rect_t top_row = {
            .x = bounds->x + (int32_t)inset,
            .y = bounds->y + (int32_t)row,
            .width = width,
            .height = 1U
        };

        ui_rect_t bottom_row = {
            .x = bounds->x + (int32_t)inset,
            .y = bounds->y +
                (int32_t)bounds->height -
                (int32_t)row - 1,
            .width = width,
            .height = 1U
        };

        ui_fill_rect(&top_row, color);
        ui_fill_rect(&bottom_row, color);
    }
}

void visual_effects_fill_top_rounded_rect(
    const ui_rect_t *bounds,
    uint32_t radius,
    uint32_t color
)
{
    if (
        bounds == NULL ||
        bounds->width == 0U ||
        bounds->height == 0U
    )
    {
        return;
    }

    if (radius > bounds->height)
    {
        radius = bounds->height;
    }

    if (radius * 2U > bounds->width)
    {
        radius = bounds->width / 2U;
    }

    ui_rect_t body = *bounds;
    body.y += (int32_t)radius;
    body.height -= radius;
    ui_fill_rect(&body, color);

    for (uint32_t row = 0; row < radius; row++)
    {
        uint32_t inset = rounded_inset_for_row(radius, row);
        uint32_t width = bounds->width > inset * 2U ?
            bounds->width - inset * 2U : 0U;

        if (width == 0U)
        {
            continue;
        }

        ui_rect_t line = {
            .x = bounds->x + (int32_t)inset,
            .y = bounds->y + (int32_t)row,
            .width = width,
            .height = 1U
        };

        ui_fill_rect(&line, color);
    }
}

void visual_effects_draw_rounded_border(
    const ui_rect_t *bounds,
    uint32_t radius,
    uint32_t thickness,
    uint32_t border_color,
    uint32_t interior_color
)
{
    if (
        bounds == NULL ||
        thickness == 0U ||
        bounds->width <= thickness * 2U ||
        bounds->height <= thickness * 2U
    )
    {
        return;
    }

    visual_effects_fill_rounded_rect(
        bounds,
        radius,
        border_color
    );

    ui_rect_t inner = ui_inset(
        bounds,
        (int32_t)thickness,
        (int32_t)thickness
    );

    uint32_t inner_radius = radius > thickness ?
        radius - thickness : 0U;

    visual_effects_fill_rounded_rect(
        &inner,
        inner_radius,
        interior_color
    );
}

void visual_effects_draw_snap_preview(
    const ui_rect_t *bounds,
    uint32_t color
)
{
    if (bounds == NULL)
    {
        return;
    }

    visual_effects_blend_rect(bounds, color, 52U);

    ui_rect_t inset = ui_inset(bounds, 5, 5);
    ui_draw_border(&inset, color, 3U);
}
