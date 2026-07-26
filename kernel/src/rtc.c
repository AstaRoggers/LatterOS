#include "rtc.h"

#include "io.h"

#include <stdbool.h>
#include <stdint.h>

#define CMOS_ADDRESS_PORT 0x70
#define CMOS_DATA_PORT    0x71

#define CMOS_SECONDS      0x00
#define CMOS_MINUTES      0x02
#define CMOS_HOURS        0x04
#define CMOS_DAY          0x07
#define CMOS_MONTH        0x08
#define CMOS_YEAR         0x09
#define CMOS_STATUS_A     0x0A
#define CMOS_STATUS_B     0x0B
#define CMOS_CENTURY      0x32

#define CMOS_UPDATE_IN_PROGRESS 0x80
#define CMOS_BINARY_MODE        0x04
#define CMOS_24_HOUR_MODE       0x02
#define CMOS_PM_FLAG            0x80

#define RTC_STABLE_ATTEMPTS 8
#define RTC_UPDATE_TIMEOUT  1000000U

typedef struct
{
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint8_t year;
    uint8_t century;
} rtc_raw_datetime_t;

static uint8_t cmos_read(uint8_t index)
{
    outb(
        CMOS_ADDRESS_PORT,
        index
    );

    io_wait();

    return inb(CMOS_DATA_PORT);
}

static bool wait_for_update(void)
{
    for (
        uint32_t timeout = 0;
        timeout < RTC_UPDATE_TIMEOUT;
        timeout++
    )
    {
        if (
            !(cmos_read(CMOS_STATUS_A) &
            CMOS_UPDATE_IN_PROGRESS)
        )
        {
            return true;
        }

        __asm__ volatile("pause");
    }

    return false;
}

static rtc_raw_datetime_t read_raw(void)
{
    rtc_raw_datetime_t value;

    value.second = cmos_read(CMOS_SECONDS);
    value.minute = cmos_read(CMOS_MINUTES);
    value.hour = cmos_read(CMOS_HOURS);
    value.day = cmos_read(CMOS_DAY);
    value.month = cmos_read(CMOS_MONTH);
    value.year = cmos_read(CMOS_YEAR);
    value.century = cmos_read(CMOS_CENTURY);

    return value;
}

static bool raw_equal(
    const rtc_raw_datetime_t *first,
    const rtc_raw_datetime_t *second
)
{
    return
        first->second == second->second &&
        first->minute == second->minute &&
        first->hour == second->hour &&
        first->day == second->day &&
        first->month == second->month &&
        first->year == second->year &&
        first->century == second->century;
}

static uint8_t bcd_to_binary(uint8_t value)
{
    return
        (uint8_t)(
            (value & 0x0F) +
            ((value >> 4) * 10)
        );
}

bool rtc_read(rtc_datetime_t *datetime)
{
    if (datetime == 0)
    {
        return false;
    }

    rtc_raw_datetime_t first;
    rtc_raw_datetime_t second;
    bool stable = false;

    for (
        uint8_t attempt = 0;
        attempt < RTC_STABLE_ATTEMPTS;
        attempt++
    )
    {
        if (!wait_for_update())
        {
            return false;
        }

        first = read_raw();

        if (!wait_for_update())
        {
            return false;
        }

        second = read_raw();

        if (raw_equal(&first, &second))
        {
            stable = true;
            break;
        }
    }

    if (!stable)
    {
        return false;
    }

    uint8_t status_b =
        cmos_read(CMOS_STATUS_B);

    bool pm =
        (second.hour & CMOS_PM_FLAG) != 0;

    second.hour &=
        (uint8_t)~CMOS_PM_FLAG;

    if (!(status_b & CMOS_BINARY_MODE))
    {
        second.second =
            bcd_to_binary(second.second);

        second.minute =
            bcd_to_binary(second.minute);

        second.hour =
            bcd_to_binary(second.hour);

        second.day =
            bcd_to_binary(second.day);

        second.month =
            bcd_to_binary(second.month);

        second.year =
            bcd_to_binary(second.year);

        if (
            second.century != 0 &&
            second.century != 0xFF
        )
        {
            second.century =
                bcd_to_binary(second.century);
        }
    }

    if (!(status_b & CMOS_24_HOUR_MODE))
    {
        second.hour =
            (uint8_t)(second.hour % 12);

        if (pm)
        {
            second.hour += 12;
        }
    }

    uint16_t full_year;

    if (
        second.century == 0 ||
        second.century == 0xFF
    )
    {
        full_year =
            (uint16_t)(2000 + second.year);
    }
    else
    {
        full_year =
            (uint16_t)(
                second.century * 100 +
                second.year
            );
    }

    if (
        second.month < 1 ||
        second.month > 12 ||
        second.day < 1 ||
        second.day > 31 ||
        second.hour > 23 ||
        second.minute > 59 ||
        second.second > 59
    )
    {
        return false;
    }

    datetime->year = full_year;
    datetime->month = second.month;
    datetime->day = second.day;
    datetime->hour = second.hour;
    datetime->minute = second.minute;
    datetime->second = second.second;

    return true;
}
