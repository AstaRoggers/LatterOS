#ifndef SPEAKER_H
#define SPEAKER_H

#include <stdbool.h>
#include <stdint.h>

void speaker_init(void);
void speaker_start(uint32_t frequency);
void speaker_stop(void);
void speaker_beep(
    uint32_t frequency,
    uint32_t duration_ms
);
bool speaker_is_active(void);

#endif
