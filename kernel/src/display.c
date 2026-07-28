#include "display.h"

#include "boot_mode.h"
#include "graphics.h"
#include "platform_detect.h"
#include "surface.h"
#include "timer.h"
#include "virtio_gpu.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DISPLAY_MAX_WIDTH 1280U
#define DISPLAY_MAX_HEIGHT 800U
#define DISPLAY_MAX_PIXELS \
    ((uint64_t)DISPLAY_MAX_WIDTH * DISPLAY_MAX_HEIGHT)
#define DISPLAY_FRAME_HISTORY 64U
#define DISPLAY_DEFAULT_REFRESH_HZ 60U

typedef struct
{
    surface_t surface;
    ui_rect_t damage[DISPLAY_MAX_DAMAGE_RECTS];
    uint32_t damage_count;
    uint64_t sequence;
    bool ready;
} scanout_slot_t;

static uint32_t scanout_pixels[
    DISPLAY_SCANOUT_BUFFER_COUNT
][DISPLAY_MAX_PIXELS] __attribute__((aligned(64)));

static scanout_slot_t slots[DISPLAY_SCANOUT_BUFFER_COUNT];
static display_backend_t active_backend;
static uint32_t active_width;
static uint32_t active_height;
static uint32_t refresh_rate;
static bool initialized;
static bool buffering_available;
static uint32_t next_slot;
static uint32_t pending_slot;
static uint64_t next_sequence;
static uint64_t last_present_tick;
static uint64_t statistics_started_at;
static uint64_t queued_frames;
static uint64_t presented_frames;
static uint64_t dropped_frames;
static uint64_t vsync_events;
static uint64_t captured_pixels;
static uint64_t presented_pixels;
static uint64_t capture_failures;
static uint64_t present_ticks;
static uint64_t frame_history[DISPLAY_FRAME_HISTORY];
static uint32_t frame_history_count;
static uint32_t frame_history_next;

static uint64_t rectangle_area(const ui_rect_t *rectangle)
{
    if (rectangle == NULL)
    {
        return 0;
    }

    return (uint64_t)rectangle->width * rectangle->height;
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
        left >= active_width ||
        top >= active_height
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

    if (right > active_width)
    {
        right = active_width;
    }

    if (bottom > active_height)
    {
        bottom = active_height;
    }

    output->x = (int32_t)left;
    output->y = (int32_t)top;
    output->width = (uint32_t)(right - left);
    output->height = (uint32_t)(bottom - top);
    return output->width != 0U && output->height != 0U;
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

    ui_rect_t result = {
        .x = left,
        .y = top,
        .width = (uint32_t)(right - left),
        .height = (uint32_t)(bottom - top)
    };

    return result;
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

static void add_damage_rectangle(
    ui_rect_t rectangles[DISPLAY_MAX_DAMAGE_RECTS],
    uint32_t *count,
    const ui_rect_t *rectangle
)
{
    if (rectangles == NULL || count == NULL || rectangle == NULL)
    {
        return;
    }

    ui_rect_t normalized;

    if (!normalize_rectangle(rectangle, &normalized))
    {
        return;
    }

    for (uint32_t index = 0; index < *count; index++)
    {
        if (!rectangles_touch(&rectangles[index], &normalized))
        {
            continue;
        }

        rectangles[index] = rectangle_union(
            &rectangles[index],
            &normalized
        );

        /*
         * The enlarged union may now overlap other entries. Collapse them
         * so stale positions from every superseded frame remain damaged.
         */
        for (uint32_t other = 0; other < *count;)
        {
            if (other == index ||
                !rectangles_touch(&rectangles[index], &rectangles[other]))
            {
                other++;
                continue;
            }

            rectangles[index] = rectangle_union(
                &rectangles[index],
                &rectangles[other]
            );

            (*count)--;
            rectangles[other] = rectangles[*count];

            if (other < index)
            {
                index--;
            }
        }

        return;
    }

    if (*count < DISPLAY_MAX_DAMAGE_RECTS)
    {
        rectangles[*count] = normalized;
        (*count)++;
        return;
    }

    /* Too many independent regions: a full frame is safer than a ghost. */
    rectangles[0].x = 0;
    rectangles[0].y = 0;
    rectangles[0].width = active_width;
    rectangles[0].height = active_height;
    *count = 1U;
}

static uint64_t refresh_period_ticks(void)
{
    uint32_t frequency = timer_frequency();

    if (frequency == 0U || refresh_rate == 0U)
    {
        return 1U;
    }

    uint64_t period = frequency / refresh_rate;
    return period == 0U ? 1U : period;
}

static bool presentation_due(void)
{
    uint64_t now = timer_ticks();

    if (presented_frames == 0U || last_present_tick == 0U)
    {
        return true;
    }

    return now - last_present_tick >= refresh_period_ticks();
}

static void initialize_slot(uint32_t index)
{
    slots[index].damage_count = 0;
    slots[index].sequence = 0;
    slots[index].ready = false;

    (void)surface_init(
        &slots[index].surface,
        scanout_pixels[index],
        active_width,
        active_height,
        active_width
    );
}

void display_init(void)
{
    active_backend = DISPLAY_BACKEND_SOFTWARE_FRAMEBUFFER;
    active_width = graphics_width();
    active_height = graphics_height();
    refresh_rate = DISPLAY_DEFAULT_REFRESH_HZ;
    next_slot = 0U;
    pending_slot = UINT32_MAX;
    next_sequence = 1U;
    last_present_tick = 0U;

    buffering_available =
        active_width != 0U &&
        active_height != 0U &&
        active_width <= DISPLAY_MAX_WIDTH &&
        active_height <= DISPLAY_MAX_HEIGHT &&
        (uint64_t)active_width * active_height <= DISPLAY_MAX_PIXELS;

    for (
        uint32_t index = 0;
        index < DISPLAY_SCANOUT_BUFFER_COUNT;
        index++
    )
    {
        initialize_slot(index);
    }

    platform_detect_init();

    if (
        buffering_available &&
        !boot_mode_conservative_graphics() &&
        !platform_is_virtualbox() &&
        virtio_gpu_init(active_width, active_height)
    )
    {
        active_backend = DISPLAY_BACKEND_VIRTIO_GPU;
    }

    initialized = true;
    display_reset_statistics();
}

void display_set_refresh_rate(uint32_t refresh_hz)
{
    if (!initialized)
    {
        display_init();
    }

    if (refresh_hz < 30U)
    {
        refresh_hz = 30U;
    }

    if (refresh_hz > 240U)
    {
        refresh_hz = 240U;
    }

    refresh_rate = refresh_hz;
}

bool display_queue_frame(
    const ui_rect_t *rectangles,
    uint32_t rectangle_count
)
{
    if (!initialized)
    {
        display_init();
    }

    if (
        rectangles == NULL ||
        rectangle_count == 0U
    )
    {
        return false;
    }

    ui_rect_t accumulated[DISPLAY_MAX_DAMAGE_RECTS];
    uint32_t accumulated_count = 0U;
    uint32_t replaced_slot = pending_slot;

    /*
     * A queued frame may be superseded before its VSync deadline. Preserve
     * all of that frame's damage in the replacement. Otherwise the logical
     * window position advances while the visible framebuffer still contains
     * an older position, producing drag trails until mouse release.
     */
    if (
        replaced_slot != UINT32_MAX &&
        slots[replaced_slot].ready
    )
    {
        for (
            uint32_t index = 0;
            index < slots[replaced_slot].damage_count;
            index++
        )
        {
            add_damage_rectangle(
                accumulated,
                &accumulated_count,
                &slots[replaced_slot].damage[index]
            );
        }
    }

    uint32_t maximum = rectangle_count;

    if (maximum > DISPLAY_MAX_DAMAGE_RECTS)
    {
        maximum = DISPLAY_MAX_DAMAGE_RECTS;
    }

    for (uint32_t index = 0; index < maximum; index++)
    {
        add_damage_rectangle(
            accumulated,
            &accumulated_count,
            &rectangles[index]
        );
    }

    if (accumulated_count == 0U)
    {
        return false;
    }

    scanout_slot_t *slot = &slots[next_slot];
    slot->damage_count = 0U;
    slot->sequence = next_sequence++;
    slot->ready = false;

    for (uint32_t index = 0; index < accumulated_count; index++)
    {
        const ui_rect_t *normalized = &accumulated[index];

        if (buffering_available)
        {
            uint32_t *destination =
                &slot->surface.pixels[
                    (uint64_t)(uint32_t)normalized->y *
                        slot->surface.stride +
                    (uint32_t)normalized->x
                ];

            if (!graphics_capture_rectangle(
                (uint32_t)normalized->x,
                (uint32_t)normalized->y,
                normalized->width,
                normalized->height,
                destination,
                slot->surface.stride
            ))
            {
                capture_failures++;
                slot->damage_count = 0U;
                return false;
            }
        }

        slot->damage[slot->damage_count] = *normalized;
        slot->damage_count++;
        captured_pixels += rectangle_area(normalized);
    }

    if (replaced_slot != UINT32_MAX)
    {
        slots[replaced_slot].ready = false;
        slots[replaced_slot].damage_count = 0U;
        dropped_frames++;
    }

    slot->ready = true;
    pending_slot = next_slot;
    next_slot = (next_slot + 1U) % DISPLAY_SCANOUT_BUFFER_COUNT;
    queued_frames++;
    return true;
}

static void present_pending_slot(void)
{
    if (pending_slot == UINT32_MAX)
    {
        return;
    }

    scanout_slot_t *slot = &slots[pending_slot];

    if (!slot->ready)
    {
        pending_slot = UINT32_MAX;
        return;
    }

    uint64_t started = timer_ticks();
    graphics_reset_clip();

    for (uint32_t index = 0; index < slot->damage_count; index++)
    {
        const ui_rect_t *rectangle = &slot->damage[index];

        if (buffering_available)
        {
            const uint32_t *source =
                &slot->surface.pixels[
                    (uint64_t)(uint32_t)rectangle->y *
                        slot->surface.stride +
                    (uint32_t)rectangle->x
                ];

            graphics_blit_surface(
                source,
                slot->surface.stride,
                rectangle->width,
                rectangle->height,
                rectangle->x,
                rectangle->y
            );
        }

        /*
         * Keep the Limine framebuffer current even while Virtio-GPU owns the
         * visible scanout. This preserves a complete software fallback and
         * makes switching back to the boot framebuffer deterministic.
         */
        graphics_present_rectangle(
            (uint32_t)rectangle->x,
            (uint32_t)rectangle->y,
            rectangle->width,
            rectangle->height
        );

        presented_pixels += rectangle_area(rectangle);
    }

    if (
        active_backend == DISPLAY_BACKEND_VIRTIO_GPU &&
        buffering_available
    )
    {
        if (!virtio_gpu_present(
            slot->surface.pixels,
            slot->surface.stride,
            slot->damage,
            slot->damage_count
        ))
        {
            capture_failures++;
        }
    }

    graphics_cursor_refresh();

    uint64_t completed = timer_ticks();
    present_ticks += completed - started;
    last_present_tick = completed;
    vsync_events++;
    presented_frames++;

    frame_history[frame_history_next] = completed;
    frame_history_next =
        (frame_history_next + 1U) % DISPLAY_FRAME_HISTORY;

    if (frame_history_count < DISPLAY_FRAME_HISTORY)
    {
        frame_history_count++;
    }

    slot->ready = false;
    slot->damage_count = 0U;
    pending_slot = UINT32_MAX;
}

void display_update(void)
{
    if (!initialized)
    {
        display_init();
    }

    if (pending_slot != UINT32_MAX && presentation_due())
    {
        present_pending_slot();
    }
}

void display_force_present(void)
{
    if (!initialized)
    {
        display_init();
    }

    present_pending_slot();
}

void display_cancel_pending(void)
{
    if (pending_slot != UINT32_MAX)
    {
        slots[pending_slot].ready = false;
        slots[pending_slot].damage_count = 0U;
        pending_slot = UINT32_MAX;
    }
}

bool display_triple_buffered(void)
{
    return buffering_available;
}

bool display_frame_pending(void)
{
    return pending_slot != UINT32_MAX;
}

const char *display_backend_name(void)
{
    switch (active_backend)
    {
        case DISPLAY_BACKEND_VIRTIO_GPU:
            return "virtio-gpu";

        case DISPLAY_BACKEND_SOFTWARE_FRAMEBUFFER:
        default:
            if (platform_is_virtualbox())
            {
                return buffering_available ?
                    "virtualbox-efi-scanout" :
                    "virtualbox-efi-framebuffer";
            }

            return buffering_available ?
                "software-scanout" :
                "direct-framebuffer";
    }
}

uint32_t display_scanout_buffer_count(void)
{
    return buffering_available ? DISPLAY_SCANOUT_BUFFER_COUNT : 1U;
}

uint32_t display_refresh_rate(void)
{
    return refresh_rate;
}

uint64_t display_presented_frames(void)
{
    return presented_frames;
}

uint64_t display_dropped_frames(void)
{
    return dropped_frames;
}

uint64_t display_vsync_events(void)
{
    return vsync_events;
}

uint32_t display_recent_fps(void)
{
    if (frame_history_count < 2U)
    {
        return 0U;
    }

    uint32_t oldest_index =
        frame_history_count < DISPLAY_FRAME_HISTORY ?
            0U : frame_history_next;

    uint32_t newest_index =
        (frame_history_next + DISPLAY_FRAME_HISTORY - 1U) %
        DISPLAY_FRAME_HISTORY;

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

uint64_t display_average_present_ms(void)
{
    uint32_t frequency = timer_frequency();

    if (presented_frames == 0U || frequency == 0U)
    {
        return 0U;
    }

    return
        present_ticks * 1000ULL /
        frequency /
        presented_frames;
}

void display_get_stats(display_stats_t *statistics)
{
    if (statistics == NULL)
    {
        return;
    }

    statistics->backend = active_backend;
    statistics->width = active_width;
    statistics->height = active_height;
    statistics->refresh_hz = refresh_rate;
    statistics->scanout_buffers = display_scanout_buffer_count();
    statistics->triple_buffered = buffering_available;
    statistics->frame_pending = pending_slot != UINT32_MAX;
    statistics->queued_frames = queued_frames;
    statistics->presented_frames = presented_frames;
    statistics->dropped_frames = dropped_frames;
    statistics->vsync_events = vsync_events;
    statistics->captured_pixels = captured_pixels;
    statistics->presented_pixels = presented_pixels;
    statistics->capture_failures = capture_failures;
    statistics->present_ticks = present_ticks;
}

void display_reset_statistics(void)
{
    statistics_started_at = timer_ticks();
    queued_frames = 0U;
    presented_frames = 0U;
    dropped_frames = 0U;
    vsync_events = 0U;
    captured_pixels = 0U;
    presented_pixels = 0U;
    capture_failures = 0U;
    present_ticks = 0U;
    frame_history_count = 0U;
    frame_history_next = 0U;

    for (uint32_t index = 0; index < DISPLAY_FRAME_HISTORY; index++)
    {
        frame_history[index] = 0U;
    }

    (void)statistics_started_at;
}
