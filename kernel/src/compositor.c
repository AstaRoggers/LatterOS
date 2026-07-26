#include "compositor.h"

#include "graphics.h"
#include "timer.h"

#include <stddef.h>
#include <stdint.h>

#define COMPOSITOR_MAX_DIRTY_RECTS 24
#define COMPOSITOR_FRAME_HISTORY 64

static compositor_render_function_t render_function;

static ui_rect_t dirty_rectangles[
    COMPOSITOR_MAX_DIRTY_RECTS
];

static uint32_t dirty_count;

static uint64_t rendered_frames;
static uint64_t rendered_rectangles;
static uint64_t rendered_pixels;
static uint64_t statistics_started_at;
static uint64_t render_ticks;
static uint64_t present_ticks;
static uint64_t frame_history[COMPOSITOR_FRAME_HISTORY];
static uint32_t frame_history_count;
static uint32_t frame_history_next;

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
    compositor_reset_statistics();
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

    rendered_frames++;
    rendered_rectangles += count;

    for (uint32_t index = 0; index < count; index++)
    {
        rendered_pixels +=
            rectangle_area(&dirty_rectangles[index]);
    }

    uint64_t started_at = timer_ticks();

    /*
     * Finish every dirty region in the software backbuffer before touching
     * the visible framebuffer. Presenting while still rendering another
     * region can expose half-updated cursor and window positions.
     */
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
    }

    graphics_reset_clip();

    uint64_t rendered_at = timer_ticks();
    render_ticks += rendered_at - started_at;

    for (uint32_t index = 0; index < count; index++)
    {
        const ui_rect_t *rectangle =
            &dirty_rectangles[index];

        graphics_present_rectangle(
            (uint32_t)rectangle->x,
            (uint32_t)rectangle->y,
            rectangle->width,
            rectangle->height
        );
    }

    uint64_t presented_at = timer_ticks();
    present_ticks += presented_at - rendered_at;

    frame_history[frame_history_next] = presented_at;
    frame_history_next =
        (frame_history_next + 1) % COMPOSITOR_FRAME_HISTORY;

    if (frame_history_count < COMPOSITOR_FRAME_HISTORY)
    {
        frame_history_count++;
    }

    /* The hardware cursor overlay is redrawn after all damaged pixels. */
    graphics_cursor_refresh();
}

uint64_t compositor_frame_count(void)
{
    return rendered_frames;
}

uint64_t compositor_rectangle_count(void)
{
    return rendered_rectangles;
}

uint64_t compositor_pixel_count(void)
{
    return rendered_pixels;
}

uint32_t compositor_average_fps(void)
{
    uint64_t elapsed =
        timer_ticks() - statistics_started_at;

    uint32_t frequency = timer_frequency();

    if (elapsed == 0 || frequency == 0)
    {
        return 0;
    }

    return (uint32_t)(
        rendered_frames * frequency / elapsed
    );
}


uint32_t compositor_recent_active_fps(void)
{
    if (frame_history_count < 2)
    {
        return 0;
    }

    uint32_t oldest_index;

    if (frame_history_count < COMPOSITOR_FRAME_HISTORY)
    {
        oldest_index = 0;
    }
    else
    {
        oldest_index = frame_history_next;
    }

    uint32_t newest_index =
        (frame_history_next + COMPOSITOR_FRAME_HISTORY - 1) %
        COMPOSITOR_FRAME_HISTORY;

    uint64_t elapsed =
        frame_history[newest_index] -
        frame_history[oldest_index];

    uint32_t frequency = timer_frequency();

    if (elapsed == 0 || frequency == 0)
    {
        return 0;
    }

    return (uint32_t)(
        (uint64_t)(frame_history_count - 1) *
        frequency /
        elapsed
    );
}

uint64_t compositor_average_render_ms(void)
{
    if (rendered_frames == 0)
    {
        return 0;
    }

    uint32_t frequency = timer_frequency();

    if (frequency == 0)
    {
        return 0;
    }

    return
        render_ticks * 1000 /
        frequency /
        rendered_frames;
}

uint64_t compositor_average_present_ms(void)
{
    if (rendered_frames == 0)
    {
        return 0;
    }

    uint32_t frequency = timer_frequency();

    if (frequency == 0)
    {
        return 0;
    }

    return
        present_ticks * 1000 /
        frequency /
        rendered_frames;
}

void compositor_reset_statistics(void)
{
    rendered_frames = 0;
    rendered_rectangles = 0;
    rendered_pixels = 0;
    statistics_started_at = timer_ticks();
    render_ticks = 0;
    present_ticks = 0;
    frame_history_count = 0;
    frame_history_next = 0;

    for (
        uint32_t index = 0;
        index < COMPOSITOR_FRAME_HISTORY;
        index++
    )
    {
        frame_history[index] = 0;
    }
}
