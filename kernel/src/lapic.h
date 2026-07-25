#ifndef LAPIC_H
#define LAPIC_H

#include <stdbool.h>

bool lapic_init(void);
bool lapic_is_enabled(void);
void lapic_send_eoi(void);

#endif
