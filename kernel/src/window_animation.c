#include "window_animation.h"

#include "timer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct
{
    bool active;
    window_animation_type_t type;
    ui_rect_t from;
    ui_rect_t to;
    ui_rect_t current;
    uint64_t started_at;
    uint64_t duration_ticks;
} animation_state_t;

static animation_state_t animations[WINDOW_ANIMATION_COUNT];

static uint64_t duration_to_ticks(uint32_t duration_ms)
{
    uint32_t frequency = timer_frequency();

    if (frequency == 0U)
    {
        return duration_ms == 0U ? 1U : duration_ms;
    }

    uint64_t ticks =
        ((uint64_t)frequency * duration_ms + 999U) /
        1000U;

    return ticks == 0U ? 1U : ticks;
}

static int32_t interpolate_i32(
    int32_t from,
    int32_t to,
    uint32_t progress
)
{
    int64_t difference = (int64_t)to - from;
    return (int32_t)(from + difference * progress / 1024);
}

static uint32_t interpolate_u32(
    uint32_t from,
    uint32_t to,
    uint32_t progress
)
{
    int64_t difference = (int64_t)to - from;
    int64_t value = (int64_t)from + difference * progress / 1024;
    return value > 0 ? (uint32_t)value : 1U;
}

static uint32_t ease_out_cubic(uint32_t progress)
{
    if (progress >= 1024U)
    {
        return 1024U;
    }

    uint64_t inverse = 1024U - progress;
    uint64_t cubic = inverse * inverse * inverse;
    uint64_t remaining = cubic / (1024ULL * 1024ULL);
    return (uint32_t)(1024U - remaining);
}

static ui_rect_t interpolate_rect(
    const ui_rect_t *from,
    const ui_rect_t *to,
    uint32_t progress
)
{
    ui_rect_t result = {
        .x = interpolate_i32(from->x, to->x, progress),
        .y = interpolate_i32(from->y, to->y, progress),
        .width = interpolate_u32(from->width, to->width, progress),
        .height = interpolate_u32(from->height, to->height, progress)
    };

    return result;
}

void window_animation_init(void)
{
    for (uint32_t index = 0; index < WINDOW_ANIMATION_COUNT; index++)
    {
        animations[index] = (animation_state_t){ 0 };
    }
}

bool window_animation_start(
    uint32_t window_id,
    window_animation_type_t type,
    const ui_rect_t *from,
    const ui_rect_t *to,
    uint32_t duration_ms
)
{
    if (
        window_id >= WINDOW_ANIMATION_COUNT ||
        type == WINDOW_ANIMATION_NONE ||
        from == NULL ||
        to == NULL ||
        from->width == 0U ||
        from->height == 0U ||
        to->width == 0U ||
        to->height == 0U
    )
    {
        return false;
    }

    animation_state_t *animation = &animations[window_id];
    animation->active = true;
    animation->type = type;
    animation->from = *from;
    animation->to = *to;
    animation->current = *from;
    animation->started_at = timer_ticks();
    animation->duration_ticks = duration_to_ticks(duration_ms);
    return true;
}

void window_animation_cancel(uint32_t window_id)
{
    if (window_id >= WINDOW_ANIMATION_COUNT)
    {
        return;
    }

    animations[window_id].active = false;
    animations[window_id].type = WINDOW_ANIMATION_NONE;
}

bool window_animation_active(uint32_t window_id)
{
    return
        window_id < WINDOW_ANIMATION_COUNT &&
        animations[window_id].active;
}

window_animation_type_t window_animation_type(uint32_t window_id)
{
    if (window_id >= WINDOW_ANIMATION_COUNT)
    {
        return WINDOW_ANIMATION_NONE;
    }

    return animations[window_id].type;
}

bool window_animation_bounds(
    uint32_t window_id,
    ui_rect_t *bounds
)
{
    if (
        window_id >= WINDOW_ANIMATION_COUNT ||
        bounds == NULL ||
        !animations[window_id].active
    )
    {
        return false;
    }

    *bounds = animations[window_id].current;
    return true;
}

bool window_animation_step(
    uint32_t window_id,
    ui_rect_t *previous,
    ui_rect_t *current,
    bool *finished
)
{
    if (
        window_id >= WINDOW_ANIMATION_COUNT ||
        previous == NULL ||
        current == NULL ||
        finished == NULL ||
        !animations[window_id].active
    )
    {
        return false;
    }

    animation_state_t *animation = &animations[window_id];
    *previous = animation->current;

    uint64_t elapsed = timer_ticks() - animation->started_at;
    uint32_t linear = elapsed >= animation->duration_ticks ?
        1024U :
        (uint32_t)(elapsed * 1024U / animation->duration_ticks);

    uint32_t eased = ease_out_cubic(linear);
    animation->current = interpolate_rect(
        &animation->from,
        &animation->to,
        eased
    );

    *current = animation->current;
    *finished = linear >= 1024U;

    if (*finished)
    {
        animation->current = animation->to;
        *current = animation->to;
        animation->active = false;
    }

    return true;
}
