#include "compositor.h"

#include "graphics.h"

#include <stddef.h>
#include <stdint.h>

#define COMPOSITOR_MAX_DIRTY_RECTS 24

static compositor_render_function_t render_function;

static ui_rect_t dirty_rectangles[
    COMPOSITOR_MAX_DIRTY_RECTS
];

static uint32_t dirty_count;

static uint64_t rectangle_area(
    const ui_rect_t *rectangle
)
{
    return
        (uint64_t)rectangle->width *
        rectangle->height;
}

static bool normalize_rectangle(
    const ui_rect_t *input,
    ui_rect_t *output
)
{
    if (
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
    int64_t right =
        left + input->width;

    int64_t bottom =
        top + input->height;

    if (
        right <= 0 ||
        bottom <= 0 ||
        left >= graphics_width() ||
        top >= graphics_height()
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

    if (right > graphics_width())
    {
        right = graphics_width();
    }

    if (bottom > graphics_height())
    {
        bottom = graphics_height();
    }

    output->x = (int32_t)left;
    output->y = (int32_t)top;
    output->width = (uint32_t)(right - left);
    output->height = (uint32_t)(bottom - top);

    return
        output->width != 0 &&
        output->height != 0;
}

static bool rectangles_touch(
    const ui_rect_t *first,
    const ui_rect_t *second
)
{
    int64_t first_right =
        (int64_t)first->x + first->width;

    int64_t first_bottom =
        (int64_t)first->y + first->height;

    int64_t second_right =
        (int64_t)second->x + second->width;

    int64_t second_bottom =
        (int64_t)second->y + second->height;

    return
        first->x <= second_right + 2 &&
        second->x <= first_right + 2 &&
        first->y <= second_bottom + 2 &&
        second->y <= first_bottom + 2;
}

static ui_rect_t rectangle_union(
    const ui_rect_t *first,
    const ui_rect_t *second
)
{
    int32_t left =
        first->x < second->x ?
            first->x : second->x;

    int32_t top =
        first->y < second->y ?
            first->y : second->y;

    int64_t first_right =
        (int64_t)first->x + first->width;

    int64_t second_right =
        (int64_t)second->x + second->width;

    int64_t first_bottom =
        (int64_t)first->y + first->height;

    int64_t second_bottom =
        (int64_t)second->y + second->height;

    int64_t right =
        first_right > second_right ?
            first_right : second_right;

    int64_t bottom =
        first_bottom > second_bottom ?
            first_bottom : second_bottom;

    ui_rect_t result = {
        .x = left,
        .y = top,
        .width = (uint32_t)(right - left),
        .height = (uint32_t)(bottom - top)
    };

    return result;
}

void compositor_init(
    compositor_render_function_t renderer
)
{
    render_function = renderer;
    dirty_count = 0;
}

void compositor_invalidate(
    const ui_rect_t *rectangle
)
{
    ui_rect_t normalized;

    if (!normalize_rectangle(rectangle, &normalized))
    {
        return;
    }

    for (uint32_t index = 0; index < dirty_count; index++)
    {
        if (!rectangles_touch(
            &dirty_rectangles[index],
            &normalized
        ))
        {
            continue;
        }

        ui_rect_t combined =
            rectangle_union(
                &dirty_rectangles[index],
                &normalized
            );

        uint64_t separate_area =
            rectangle_area(&dirty_rectangles[index]) +
            rectangle_area(&normalized);

        uint64_t combined_area =
            rectangle_area(&combined);

        /*
         * Merge only when the union is cheaper than keeping both.
         * This prevents distant UI changes from becoming a huge redraw.
         */
        if (combined_area <= separate_area + separate_area / 3)
        {
            dirty_rectangles[index] = combined;

            /* Merge again in case the new union touches another area. */
            for (
                uint32_t other = 0;
                other < dirty_count;
                other++
            )
            {
                if (
                    other == index ||
                    !rectangles_touch(
                        &dirty_rectangles[index],
                        &dirty_rectangles[other]
                    )
                )
                {
                    continue;
                }

                ui_rect_t second_union =
                    rectangle_union(
                        &dirty_rectangles[index],
                        &dirty_rectangles[other]
                    );

                uint64_t second_separate =
                    rectangle_area(&dirty_rectangles[index]) +
                    rectangle_area(&dirty_rectangles[other]);

                if (
                    rectangle_area(&second_union) <=
                    second_separate + second_separate / 3
                )
                {
                    dirty_rectangles[index] = second_union;
                    dirty_count--;
                    dirty_rectangles[other] =
                        dirty_rectangles[dirty_count];

                    if (other < index)
                    {
                        index--;
                    }
                }
            }

            return;
        }
    }

    if (dirty_count >= COMPOSITOR_MAX_DIRTY_RECTS)
    {
        compositor_invalidate_all();
        return;
    }

    dirty_rectangles[dirty_count] = normalized;
    dirty_count++;
}

void compositor_invalidate_all(void)
{
    dirty_count = 1;

    dirty_rectangles[0].x = 0;
    dirty_rectangles[0].y = 0;
    dirty_rectangles[0].width = graphics_width();
    dirty_rectangles[0].height = graphics_height();
}

bool compositor_has_damage(void)
{
    return dirty_count != 0;
}

void compositor_render(void)
{
    if (
        render_function == NULL ||
        dirty_count == 0
    )
    {
        return;
    }

    uint32_t count = dirty_count;
    dirty_count = 0;

    for (uint32_t index = 0; index < count; index++)
    {
        const ui_rect_t *rectangle =
            &dirty_rectangles[index];

        graphics_set_clip(
            rectangle->x,
            rectangle->y,
            rectangle->width,
            rectangle->height
        );

        render_function();
        graphics_reset_clip();

        graphics_present_rectangle(
            (uint32_t)rectangle->x,
            (uint32_t)rectangle->y,
            rectangle->width,
            rectangle->height
        );
    }
}
