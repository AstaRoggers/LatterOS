#ifndef LAPIC_H
#define LAPIC_H

#include <stdbool.h>
#include <stdint.h>

bool lapic_init(void);
bool lapic_init_secondary(void);

bool lapic_is_enabled(void);
uint32_t lapic_id(void);

void lapic_set_legacy_pic(
    bool legacy_enabled
);

void lapic_send_eoi(void);

bool lapic_send_ipi(
    uint32_t destination_apic_id,
    uint8_t vector
);

bool lapic_timer_calibrate(void);

bool lapic_timer_start_periodic(
    uint8_t vector,
    uint32_t frequency
);

void lapic_timer_stop(void);
bool lapic_timer_is_calibrated(void);
uint64_t lapic_timer_base_frequency(void);

#endif
