#ifndef KLOG_H
#define KLOG_H

#include <stddef.h>
#include <stdint.h>

typedef enum
{
    KLOG_DEBUG = 0,
    KLOG_INFO,
    KLOG_WARNING,
    KLOG_ERROR,
    KLOG_PANIC
} klog_level_t;

void klog_init(void);

void klog_write(
    klog_level_t level,
    const char *component,
    const char *message
);

void klogf(
    klog_level_t level,
    const char *component,
    const char *format,
    ...
);

void klog_print(klog_level_t minimum_level);
void klog_clear(void);

size_t klog_entry_count(void);
const char *klog_level_name(klog_level_t level);

#endif
