#ifndef BOOT_HEALTH_H
#define BOOT_HEALTH_H

#include <stdbool.h>
#include <stdint.h>

void boot_health_init(void);
void boot_health_mark_desktop_ready(void);
bool boot_health_previous_incomplete(void);
uint32_t boot_health_attempt_count(void);
uint32_t boot_health_failure_count(void);
const char *boot_health_status(void);
bool boot_health_write_report(void);
const char *boot_health_report_path(void);

#endif
