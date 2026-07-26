#ifndef SELFTEST_H
#define SELFTEST_H

#include <stdbool.h>
#include <stdint.h>

bool selftest_memory(void);
bool selftest_guard_pages(void);
bool selftest_filesystem(void);
bool selftest_network(void);

bool selftest_scheduler_start(
    uint32_t worker_count,
    uint64_t iterations
);

void selftest_scheduler_print_status(void);

bool selftest_scheduler_running(void);
bool selftest_scheduler_passed(void);

#endif
