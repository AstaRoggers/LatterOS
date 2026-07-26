#include "mouse.h"

#include "io.h"
#include "irq.h"

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

static volatile int32_t mouse_x;
static volatile int32_t mouse_y;
static volatile uint8_t mouse_buttons;
static volatile uint64_t mouse_packets;

static uint8_t packet[3];
static uint8_t packet_index;
static bool available;

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

static void mouse_process_packet(void)
{
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
    available = false;
    packet_index = 0;

    mouse_x = 0;
    mouse_y = 0;
    mouse_buttons = 0;
    mouse_packets = 0;

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
        return false;
    }

    uint8_t configuration;

    if (!ps2_read_data(&configuration))
    {
        irq_unregister_handler(MOUSE_IRQ);
        return false;
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
        return false;
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
        return false;
    }

    available = true;
    return true;
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
