#include "serial.h"

#include "io.h"

#include <stdbool.h>
#include <stdint.h>

#define COM1_BASE 0x3F8

#define SERIAL_DATA_PORT          (COM1_BASE + 0)
#define SERIAL_INTERRUPT_ENABLE   (COM1_BASE + 1)
#define SERIAL_FIFO_CONTROL       (COM1_BASE + 2)
#define SERIAL_LINE_CONTROL       (COM1_BASE + 3)
#define SERIAL_MODEM_CONTROL      (COM1_BASE + 4)
#define SERIAL_LINE_STATUS        (COM1_BASE + 5)

#define SERIAL_DLAB               0x80
#define SERIAL_8N1                0x03
#define SERIAL_FIFO_ENABLE        0xC7
#define SERIAL_MODEM_NORMAL       0x0F
#define SERIAL_MODEM_LOOPBACK     0x1E
#define SERIAL_TRANSMITTER_EMPTY  0x20
#define SERIAL_DATA_READY         0x01

#define SERIAL_TIMEOUT 1000000U

static bool available;

static bool wait_for_transmitter(void)
{
    for (
        uint32_t timeout = 0;
        timeout < SERIAL_TIMEOUT;
        timeout++
    )
    {
        if (
            inb(SERIAL_LINE_STATUS) &
            SERIAL_TRANSMITTER_EMPTY
        )
        {
            return true;
        }

        __asm__ volatile("pause");
    }

    return false;
}

static bool wait_for_data(void)
{
    for (
        uint32_t timeout = 0;
        timeout < SERIAL_TIMEOUT;
        timeout++
    )
    {
        if (
            inb(SERIAL_LINE_STATUS) &
            SERIAL_DATA_READY
        )
        {
            return true;
        }

        __asm__ volatile("pause");
    }

    return false;
}

static bool loopback_test(uint8_t value)
{
    outb(
        SERIAL_MODEM_CONTROL,
        SERIAL_MODEM_LOOPBACK
    );

    if (!wait_for_transmitter())
    {
        outb(
            SERIAL_MODEM_CONTROL,
            SERIAL_MODEM_NORMAL
        );

        return false;
    }

    outb(SERIAL_DATA_PORT, value);

    if (!wait_for_data())
    {
        outb(
            SERIAL_MODEM_CONTROL,
            SERIAL_MODEM_NORMAL
        );

        return false;
    }

    uint8_t received =
        inb(SERIAL_DATA_PORT);

    outb(
        SERIAL_MODEM_CONTROL,
        SERIAL_MODEM_NORMAL
    );

    return received == value;
}

bool serial_init(void)
{
    available = false;

    outb(SERIAL_INTERRUPT_ENABLE, 0x00);

    outb(
        SERIAL_LINE_CONTROL,
        SERIAL_DLAB
    );

    /* 38400 baud from the standard 115200 Hz UART clock. */
    outb(SERIAL_DATA_PORT, 0x03);
    outb(SERIAL_INTERRUPT_ENABLE, 0x00);

    outb(
        SERIAL_LINE_CONTROL,
        SERIAL_8N1
    );

    outb(
        SERIAL_FIFO_CONTROL,
        SERIAL_FIFO_ENABLE
    );

    outb(
        SERIAL_MODEM_CONTROL,
        SERIAL_MODEM_NORMAL
    );

    available = loopback_test(0xAE);

    return available;
}

bool serial_is_available(void)
{
    return available;
}

bool serial_self_test(void)
{
    if (!available && !serial_init())
    {
        return false;
    }

    available = loopback_test(0x5A);
    return available;
}

bool serial_write_character(char character)
{
    if (!available)
    {
        return false;
    }

    if (!wait_for_transmitter())
    {
        available = false;
        return false;
    }

    outb(
        SERIAL_DATA_PORT,
        (uint8_t)character
    );

    return true;
}

void serial_write(const char *text)
{
    if (text == 0)
    {
        return;
    }

    for (
        uint32_t index = 0;
        text[index] != '\0';
        index++
    )
    {
        if (
            text[index] == '\n'
        )
        {
            if (!serial_write_character('\r'))
            {
                return;
            }
        }

        if (!serial_write_character(text[index]))
        {
            return;
        }
    }
}

void serial_write_line(const char *text)
{
    serial_write(text);
    serial_write("\n");
}
