#include "timer.h"

#include "io.h"
#include "irq.h"
#include "usb_hotplug.h"

#include <stdint.h>

#define PIT_INPUT_FREQUENCY 1193182U

#define PIT_CHANNEL_0       0x40
#define PIT_COMMAND         0x43

#define PIT_MODE_RATE       0x34

static volatile uint64_t tick_count;
static uint32_t configured_frequency;

static void timer_irq_handler(void)
{
    tick_count++;
}

void timer_init(uint32_t frequency)
{
    if (frequency == 0)
    {
        frequency = 100;
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

    configured_frequency =
        PIT_INPUT_FREQUENCY /
        divisor;

    tick_count = 0;

    irq_register_handler(
        0,
        timer_irq_handler
    );

    outb(
        PIT_COMMAND,
        PIT_MODE_RATE
    );

    outb(
        PIT_CHANNEL_0,
        (uint8_t)(divisor & 0xFF)
    );

    outb(
        PIT_CHANNEL_0,
        (uint8_t)((divisor >> 8) & 0xFF)
    );

    (void)usb_hotplug_init();
}

uint64_t timer_ticks(void)
{
    return tick_count;
}

uint32_t timer_frequency(void)
{
    return configured_frequency;
}
