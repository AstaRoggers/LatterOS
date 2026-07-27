#include "compositor.h"

#include "display.h"
#include "graphics.h"
#include "timer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define COMPOSITOR_MAX_DIRTY_RECTS 32U
#define COMPOSITOR_FRAME_HISTORY 64U

typedef struct
{
    ui_rect_t rectangle;
    uint64_t area;
} damage_entry_t;

static compositor_render_function_t render_function;
static damage_entry_t dirty_entries[COMPOSITOR_MAX_DIRTY_RECTS];
static uint32_t dirty_count;
static uint64_t rendered_frames;
static uint64_t rendered_rectangles;
static uint64_t rendered_pixels;
static uint64_t statistics_started_at;
static uint64_t render_ticks;
static uint64_t frame_history[COMPOSITOR_FRAME_HISTORY];
static uint32_t frame_history_count;
static uint32_t frame_history_next;

static uint64_t rectangle_area(const ui_rect_t *rectangle)
{
    return rectangle == NULL ?
        0U :
        (uint64_t)rectangle->width * rectangle->height;
}

static bool normalize_rectangle(
    const ui_rect_t *input,
    ui_rect_t *output
)
{
    if (
        input == NULL ||
        output == NULL ||
        input->width == 0U ||
        input->height == 0U
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
    return output->width != 0U && output->height != 0U;
}

static bool rectangles_touch(
    const ui_rect_t *first,
    const ui_rect_t *second
)
{
    int64_t first_right = (int64_t)first->x + first->width;
    int64_t first_bottom = (int64_t)first->y + first->height;
    int64_t second_right = (int64_t)second->x + second->width;
    int64_t second_bottom = (int64_t)second->y + second->height;

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
    int32_t left = first->x < second->x ? first->x : second->x;
    int32_t top = first->y < second->y ? first->y : second->y;
    int64_t first_right = (int64_t)first->x + first->width;
    int64_t second_right = (int64_t)second->x + second->width;
    int64_t first_bottom = (int64_t)first->y + first->height;
    int64_t second_bottom = (int64_t)second->y + second->height;
    int64_t right = first_right > second_right ? first_right : second_right;
    int64_t bottom = first_bottom > second_bottom ? first_bottom : second_bottom;

    ui_rect_t rectangle = {
        .x = left,
        .y = top,
        .width = (uint32_t)(right - left),
        .height = (uint32_t)(bottom - top)
    };

    return rectangle;
}

static void collapse_to_full_screen(void)
{
    dirty_count = 1U;
    dirty_entries[0].rectangle.x = 0;
    dirty_entries[0].rectangle.y = 0;
    dirty_entries[0].rectangle.width = graphics_width();
    dirty_entries[0].rectangle.height = graphics_height();
    dirty_entries[0].area = rectangle_area(&dirty_entries[0].rectangle);
}

void compositor_init(
    compositor_render_function_t renderer
)
{
    render_function = renderer;
    dirty_count = 0U;
    display_init();
    display_set_refresh_rate(60U);
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
        ui_rect_t *current = &dirty_entries[index].rectangle;

        if (!rectangles_touch(current, &normalized))
        {
            continue;
        }

        ui_rect_t combined = rectangle_union(current, &normalized);
        uint64_t separate =
            dirty_entries[index].area + rectangle_area(&normalized);
        uint64_t combined_area = rectangle_area(&combined);

        if (combined_area > separate + separate / 3U)
        {
            continue;
        }

        *current = combined;
        dirty_entries[index].area = combined_area;

        for (uint32_t other = 0; other < dirty_count; other++)
        {
            if (
                other == index ||
                !rectangles_touch(
                    &dirty_entries[index].rectangle,
                    &dirty_entries[other].rectangle
                )
            )
            {
                continue;
            }

            ui_rect_t second_union = rectangle_union(
                &dirty_entries[index].rectangle,
                &dirty_entries[other].rectangle
            );

            uint64_t second_separate =
                dirty_entries[index].area + dirty_entries[other].area;

            uint64_t second_area = rectangle_area(&second_union);

            if (second_area > second_separate + second_separate / 3U)
            {
                continue;
            }

            dirty_entries[index].rectangle = second_union;
            dirty_entries[index].area = second_area;
            dirty_count--;
            dirty_entries[other] = dirty_entries[dirty_count];

            if (other < index)
            {
                index--;
            }
        }

        return;
    }

    if (dirty_count >= COMPOSITOR_MAX_DIRTY_RECTS)
    {
        collapse_to_full_screen();
        return;
    }

    dirty_entries[dirty_count].rectangle = normalized;
    dirty_entries[dirty_count].area = rectangle_area(&normalized);
    dirty_count++;
}

void compositor_invalidate_all(void)
{
    collapse_to_full_screen();
}

bool compositor_has_damage(void)
{
    return dirty_count != 0U;
}

void compositor_render(void)
{
    display_update();

    if (
        render_function == NULL ||
        dirty_count == 0U
    )
    {
        return;
    }

    uint32_t count = dirty_count;
    ui_rect_t rectangles[COMPOSITOR_MAX_DIRTY_RECTS];

    for (uint32_t index = 0; index < count; index++)
    {
        rectangles[index] = dirty_entries[index].rectangle;
    }

    dirty_count = 0U;
    rendered_frames++;
    rendered_rectangles += count;

    for (uint32_t index = 0; index < count; index++)
    {
        rendered_pixels += rectangle_area(&rectangles[index]);
    }

    uint64_t started = timer_ticks();

    for (uint32_t index = 0; index < count; index++)
    {
        const ui_rect_t *rectangle = &rectangles[index];

        graphics_set_clip(
            rectangle->x,
            rectangle->y,
            rectangle->width,
            rectangle->height
        );

        render_function();
    }

    graphics_reset_clip();
    uint64_t rendered = timer_ticks();
    render_ticks += rendered - started;

    if (!display_queue_frame(rectangles, count))
    {
        for (uint32_t index = 0; index < count; index++)
        {
            graphics_present_rectangle(
                (uint32_t)rectangles[index].x,
                (uint32_t)rectangles[index].y,
                rectangles[index].width,
                rectangles[index].height
            );
        }

        graphics_cursor_refresh();
    }
    else
    {
        display_update();
    }

    uint64_t completed = timer_ticks();
    frame_history[frame_history_next] = completed;
    frame_history_next =
        (frame_history_next + 1U) % COMPOSITOR_FRAME_HISTORY;

    if (frame_history_count < COMPOSITOR_FRAME_HISTORY)
    {
        frame_history_count++;
    }
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
    uint64_t elapsed = timer_ticks() - statistics_started_at;
    uint32_t frequency = timer_frequency();

    if (elapsed == 0U || frequency == 0U)
    {
        return 0U;
    }

    return (uint32_t)(rendered_frames * frequency / elapsed);
}

uint32_t compositor_recent_active_fps(void)
{
    uint32_t display_fps = display_recent_fps();

    if (display_fps != 0U)
    {
        return display_fps;
    }

    if (frame_history_count < 2U)
    {
        return 0U;
    }

    uint32_t oldest_index =
        frame_history_count < COMPOSITOR_FRAME_HISTORY ?
            0U : frame_history_next;

    uint32_t newest_index =
        (frame_history_next + COMPOSITOR_FRAME_HISTORY - 1U) %
        COMPOSITOR_FRAME_HISTORY;

    uint64_t elapsed =
        frame_history[newest_index] - frame_history[oldest_index];

    uint32_t frequency = timer_frequency();

    if (elapsed == 0U || frequency == 0U)
    {
        return 0U;
    }

    return (uint32_t)(
        (uint64_t)(frame_history_count - 1U) * frequency / elapsed
    );
}

uint64_t compositor_average_render_ms(void)
{
    uint32_t frequency = timer_frequency();

    if (rendered_frames == 0U || frequency == 0U)
    {
        return 0U;
    }

    return render_ticks * 1000ULL / frequency / rendered_frames;
}

uint64_t compositor_average_present_ms(void)
{
    return display_average_present_ms();
}

const char *compositor_display_backend(void)
{
    return display_backend_name();
}

uint32_t compositor_scanout_buffer_count(void)
{
    return display_scanout_buffer_count();
}

uint64_t compositor_dropped_frame_count(void)
{
    return display_dropped_frames();
}

uint64_t compositor_vsync_count(void)
{
    return display_vsync_events();
}

bool compositor_triple_buffered(void)
{
    return display_triple_buffered();
}

void compositor_reset_statistics(void)
{
    rendered_frames = 0U;
    rendered_rectangles = 0U;
    rendered_pixels = 0U;
    statistics_started_at = timer_ticks();
    render_ticks = 0U;
    frame_history_count = 0U;
    frame_history_next = 0U;

    for (uint32_t index = 0; index < COMPOSITOR_FRAME_HISTORY; index++)
    {
        frame_history[index] = 0U;
    }

    display_reset_statistics();
}
