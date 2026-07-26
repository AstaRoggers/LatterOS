#ifndef RTC_H
#define RTC_H

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} rtc_datetime_t;

bool rtc_read(rtc_datetime_t *datetime);

#endif
