#include "mouse.h"

#include "io.h"
#include "irq.h"
#include "timer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PS2_DATA_PORT          0x60
#define PS2_STATUS_PORT        0x64
#define PS2_COMMAND_PORT       0x64

#define PS2_STATUS_OUTPUT_FULL 0x01
#define PS2_STATUS_INPUT_FULL  0x02
#define PS2_STATUS_AUX_DATA    0x20

#define PS2_ENABLE_PORT2       0xA8
#define PS2_READ_CONFIG        0x20
#define PS2_WRITE_CONFIG       0x60
#define PS2_WRITE_PORT2        0xD4

#define PS2_CONFIG_IRQ12       0x02
#define PS2_CONFIG_CLOCK2_OFF  0x20

#define MOUSE_SET_DEFAULTS     0xF6
#define MOUSE_ENABLE_STREAMING 0xF4
#define MOUSE_ACK              0xFA

#define MOUSE_IRQ              12
#define PS2_TIMEOUT            1000000U
#define USB_MOUSE_TIMEOUT_MS 500ULL

static volatile int32_t mouse_x;
static volatile int32_t mouse_y;
static volatile uint8_t mouse_buttons;
static volatile uint64_t mouse_packets;

static uint8_t packet[3];
static uint8_t packet_index;
static bool available;
static bool usb_input_active;
static bool usb_report_seen;
static uint64_t usb_last_report_tick;

static bool ps2_wait_input_empty(void)
{
    for (
        uint32_t timeout = 0;
        timeout < PS2_TIMEOUT;
        timeout++
    )
    {
        if (
            !(inb(PS2_STATUS_PORT) &
            PS2_STATUS_INPUT_FULL)
        )
        {
            return true;
        }

        __asm__ volatile("pause");
    }

    return false;
}

static bool ps2_wait_output_full(void)
{
    for (
        uint32_t timeout = 0;
        timeout < PS2_TIMEOUT;
        timeout++
    )
    {
        if (
            inb(PS2_STATUS_PORT) &
            PS2_STATUS_OUTPUT_FULL
        )
        {
            return true;
        }

        __asm__ volatile("pause");
    }

    return false;
}

static bool ps2_send_controller_command(
    uint8_t command
)
{
    if (!ps2_wait_input_empty())
    {
        return false;
    }

    outb(
        PS2_COMMAND_PORT,
        command
    );

    return true;
}

static bool ps2_write_data(uint8_t value)
{
    if (!ps2_wait_input_empty())
    {
        return false;
    }

    outb(
        PS2_DATA_PORT,
        value
    );

    return true;
}

static bool ps2_read_data(uint8_t *value)
{
    if (
        value == NULL ||
        !ps2_wait_output_full()
    )
    {
        return false;
    }

    *value = inb(PS2_DATA_PORT);
    return true;
}

static void ps2_flush_output(void)
{
    for (uint32_t count = 0; count < 64; count++)
    {
        if (
            !(inb(PS2_STATUS_PORT) &
            PS2_STATUS_OUTPUT_FULL)
        )
        {
            return;
        }

        (void)inb(PS2_DATA_PORT);
    }
}

static bool mouse_send_command(
    uint8_t command
)
{
    if (
        !ps2_send_controller_command(
            PS2_WRITE_PORT2
        ) ||
        !ps2_write_data(command)
    )
    {
        return false;
    }

    uint8_t response;

    if (!ps2_read_data(&response))
    {
        return false;
    }

    return response == MOUSE_ACK;
}

static uint64_t usb_mouse_timeout_ticks(void)
{
    uint32_t frequency = timer_frequency();

    if (frequency == 0)
    {
        return 50;
    }

    uint64_t ticks =
        ((uint64_t)frequency *
            USB_MOUSE_TIMEOUT_MS + 999ULL) /
        1000ULL;

    return ticks == 0 ? 1 : ticks;
}

static bool usb_mouse_recently_active(void)
{
    if (
        !usb_input_active ||
        !usb_report_seen
    )
    {
        return false;
    }

    uint64_t now = timer_ticks();

    return
        now - usb_last_report_tick <=
        usb_mouse_timeout_ticks();
}

static void mouse_process_packet(void)
{
    /*
     * Keep consuming PS/2 packets so the controller does not clog,
     * but prefer USB movement while valid USB reports are arriving.
     */
    if (usb_mouse_recently_active())
    {
        return;
    }

    uint8_t flags = packet[0];

    mouse_buttons =
        (uint8_t)(flags & 0x07);

    if (!(flags & 0xC0))
    {
        int32_t delta_x =
            (int32_t)(int8_t)packet[1];

        int32_t delta_y =
            (int32_t)(int8_t)packet[2];

        mouse_x += delta_x;
        mouse_y -= delta_y;
    }

    mouse_packets++;
}

static void mouse_irq_handler(void)
{
    uint8_t status =
        inb(PS2_STATUS_PORT);

    if (
        !(status & PS2_STATUS_OUTPUT_FULL) ||
        !(status & PS2_STATUS_AUX_DATA)
    )
    {
        return;
    }

    uint8_t value =
        inb(PS2_DATA_PORT);

    if (
        packet_index == 0 &&
        !(value & 0x08)
    )
    {
        return;
    }

    packet[packet_index] = value;
    packet_index++;

    if (packet_index == 3)
    {
        packet_index = 0;
        mouse_process_packet();
    }
}

bool mouse_init(void)
{
    available = usb_input_active;
    packet_index = 0;

    mouse_x = 0;
    mouse_y = 0;
    mouse_buttons = 0;
    mouse_packets = 0;

    usb_report_seen = false;
    usb_last_report_tick = 0;

    /*
     * Always initialize the PS/2 mouse as a live fallback. USB device
     * enumeration alone does not prove that interrupt reports are
     * actually arriving.
     */
    irq_register_handler(
        MOUSE_IRQ,
        mouse_irq_handler
    );

    ps2_flush_output();

    if (
        !ps2_send_controller_command(
            PS2_ENABLE_PORT2
        ) ||
        !ps2_send_controller_command(
            PS2_READ_CONFIG
        )
    )
    {
        irq_unregister_handler(MOUSE_IRQ);
        return available;
    }

    uint8_t configuration;

    if (!ps2_read_data(&configuration))
    {
        irq_unregister_handler(MOUSE_IRQ);
        return available;
    }

    configuration |=
        PS2_CONFIG_IRQ12;

    configuration &=
        (uint8_t)~PS2_CONFIG_CLOCK2_OFF;

    if (
        !ps2_send_controller_command(
            PS2_WRITE_CONFIG
        ) ||
        !ps2_write_data(configuration)
    )
    {
        irq_unregister_handler(MOUSE_IRQ);
        return available;
    }

    if (
        !mouse_send_command(
            MOUSE_SET_DEFAULTS
        ) ||
        !mouse_send_command(
            MOUSE_ENABLE_STREAMING
        )
    )
    {
        irq_unregister_handler(MOUSE_IRQ);
        return available;
    }

    available = true;
    return true;
}

void mouse_set_usb_active(bool active)
{
    usb_input_active = active;

    if (!active)
    {
        usb_report_seen = false;
        usb_last_report_tick = 0;
    }

    if (active)
    {
        available = true;
    }
}

bool mouse_usb_active(void)
{
    return usb_mouse_recently_active();
}

void mouse_handle_usb_boot_report(
    const uint8_t *report,
    uint8_t length
)
{
    if (
        !usb_input_active ||
        report == NULL ||
        length < 3
    )
    {
        return;
    }

    uint8_t buttons =
        (uint8_t)(report[0] & 0x07U);

    int32_t delta_x =
        (int32_t)(int8_t)report[1];

    int32_t delta_y =
        (int32_t)(int8_t)report[2];

    /*
     * Ignore an idle all-zero report as proof of a working USB mouse.
     * Once a real movement or button change arrives, USB becomes the
     * preferred source. If reports stop, PS/2 automatically takes over
     * again after approximately half a second at any timer rate.
     */
    if (
        delta_x == 0 &&
        delta_y == 0 &&
        buttons == mouse_buttons
    )
    {
        return;
    }

    usb_report_seen = true;
    usb_last_report_tick = timer_ticks();

    mouse_buttons = buttons;
    mouse_x += delta_x;
    mouse_y += delta_y;
    mouse_packets++;
}

bool mouse_is_available(void)
{
    return available;
}

void mouse_get_state(mouse_state_t *state)
{
    if (state == NULL)
    {
        return;
    }

    state->x = mouse_x;
    state->y = mouse_y;

    uint8_t buttons = mouse_buttons;

    state->left_button =
        (buttons & 0x01) != 0;

    state->right_button =
        (buttons & 0x02) != 0;

    state->middle_button =
        (buttons & 0x04) != 0;

    state->packet_count =
        mouse_packets;
}
