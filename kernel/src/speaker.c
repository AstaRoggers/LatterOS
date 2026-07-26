#include "speaker.h"

#include "io.h"
#include "timer.h"

#include <stdbool.h>
#include <stdint.h>

#define PIT_INPUT_FREQUENCY 1193182U
#define PIT_CHANNEL_2       0x42
#define PIT_COMMAND         0x43
#define PIT_SQUARE_WAVE     0xB6
#define SPEAKER_CONTROL     0x61
#define SPEAKER_ENABLE_BITS 0x03

static bool active;

void speaker_init(void)
{
    speaker_stop();
}

void speaker_start(uint32_t frequency)
{
    if (frequency < 20)
    {
        frequency = 20;
    }

    if (frequency > 20000)
    {
        frequency = 20000;
    }

    uint32_t divisor =
        PIT_INPUT_FREQUENCY /
        frequency;

    if (divisor < 1)
    {
        divisor = 1;
    }

    if (divisor > 0xFFFF)
    {
        divisor = 0xFFFF;
    }

    outb(
        PIT_COMMAND,
        PIT_SQUARE_WAVE
    );

    outb(
        PIT_CHANNEL_2,
        (uint8_t)(divisor & 0xFF)
    );

    outb(
        PIT_CHANNEL_2,
        (uint8_t)((divisor >> 8) & 0xFF)
    );

    uint8_t control =
        inb(SPEAKER_CONTROL);

    outb(
        SPEAKER_CONTROL,
        (uint8_t)(
            control |
            SPEAKER_ENABLE_BITS
        )
    );

    active = true;
}

void speaker_stop(void)
{
    uint8_t control =
        inb(SPEAKER_CONTROL);

    outb(
        SPEAKER_CONTROL,
        (uint8_t)(
            control &
            (uint8_t)~SPEAKER_ENABLE_BITS
        )
    );

    active = false;
}

void speaker_beep(
    uint32_t frequency,
    uint32_t duration_ms
)
{
    if (duration_ms == 0)
    {
        return;
    }

    uint32_t ticks_per_second =
        timer_frequency();

    if (ticks_per_second == 0)
    {
        return;
    }

    uint64_t duration_ticks =
        (
            (uint64_t)duration_ms *
            ticks_per_second +
            999
        ) / 1000;

    if (duration_ticks == 0)
    {
        duration_ticks = 1;
    }

    uint64_t start = timer_ticks();

    speaker_start(frequency);

    while (
        timer_ticks() - start <
        duration_ticks
    )
    {
        __asm__ volatile("pause");
    }

    speaker_stop();
}

bool speaker_is_active(void)
{
    return active;
}
