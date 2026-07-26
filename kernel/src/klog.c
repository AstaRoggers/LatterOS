#include "klog.h"

#include "kstdio.h"
#include "serial.h"
#include "terminal.h"
#include "timer.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KLOG_CAPACITY     128U
#define KLOG_MESSAGE_SIZE 192U
#define KLOG_FORMAT_SIZE  256U

typedef struct
{
    uint64_t sequence;
    uint64_t ticks;
    klog_level_t level;
    char component[24];
    char message[KLOG_MESSAGE_SIZE];
} klog_entry_t;

static klog_entry_t entries[KLOG_CAPACITY];
static size_t entry_start;
static size_t entry_count;
static uint64_t next_sequence;

static uint64_t interrupt_save(void)
{
    uint64_t flags;

    __asm__ volatile(
        "pushfq\n"
        "popq %0\n"
        "cli"
        : "=r"(flags)
        :
        : "memory"
    );

    return flags;
}

static void interrupt_restore(uint64_t flags)
{
    __asm__ volatile(
        "pushq %0\n"
        "popfq"
        :
        : "r"(flags)
        : "memory", "cc"
    );
}

static void copy_text(
    char *destination,
    size_t capacity,
    const char *source
)
{
    if (
        destination == NULL ||
        capacity == 0
    )
    {
        return;
    }

    if (source == NULL)
    {
        source = "(null)";
    }

    size_t index = 0;

    while (
        source[index] != '\0' &&
        index + 1 < capacity
    )
    {
        destination[index] = source[index];
        index++;
    }

    destination[index] = '\0';
}

const char *klog_level_name(klog_level_t level)
{
    switch (level)
    {
        case KLOG_DEBUG:
            return "DEBUG";

        case KLOG_INFO:
            return "INFO";

        case KLOG_WARNING:
            return "WARN";

        case KLOG_ERROR:
            return "ERROR";

        case KLOG_PANIC:
            return "PANIC";

        default:
            return "UNKNOWN";
    }
}

static void serial_emit(const klog_entry_t *entry)
{
    if (
        entry == NULL ||
        !serial_is_available()
    )
    {
        return;
    }

    char line[KLOG_FORMAT_SIZE];

    ksnprintf(
        line,
        sizeof(line),
        "[%06llu] %s %s: %s",
        (unsigned long long)entry->sequence,
        klog_level_name(entry->level),
        entry->component,
        entry->message
    );

    serial_write_line(line);
}

void klog_init(void)
{
    uint64_t flags = interrupt_save();

    entry_start = 0;
    entry_count = 0;
    next_sequence = 1;

    for (
        size_t index = 0;
        index < KLOG_CAPACITY;
        index++
    )
    {
        entries[index].sequence = 0;
        entries[index].ticks = 0;
        entries[index].level = KLOG_DEBUG;
        entries[index].component[0] = '\0';
        entries[index].message[0] = '\0';
    }

    interrupt_restore(flags);
}

void klog_write(
    klog_level_t level,
    const char *component,
    const char *message
)
{
    uint64_t flags = interrupt_save();

    size_t slot;

    if (entry_count < KLOG_CAPACITY)
    {
        slot =
            (entry_start + entry_count) %
            KLOG_CAPACITY;

        entry_count++;
    }
    else
    {
        slot = entry_start;
        entry_start =
            (entry_start + 1) %
            KLOG_CAPACITY;
    }

    klog_entry_t *entry = &entries[slot];

    entry->sequence = next_sequence++;
    entry->ticks = timer_ticks();
    entry->level = level;

    copy_text(
        entry->component,
        sizeof(entry->component),
        component == NULL ? "kernel" : component
    );

    copy_text(
        entry->message,
        sizeof(entry->message),
        message
    );

    klog_entry_t serial_copy = *entry;

    interrupt_restore(flags);
    serial_emit(&serial_copy);
}

void klogf(
    klog_level_t level,
    const char *component,
    const char *format,
    ...
)
{
    char message[KLOG_MESSAGE_SIZE];

    va_list arguments;
    va_start(arguments, format);

    int result = kvsnprintf(
        message,
        sizeof(message),
        format,
        arguments
    );

    va_end(arguments);

    if (result < 0)
    {
        klog_write(
            KLOG_ERROR,
            "klog",
            "Unable to format a log message"
        );

        return;
    }

    klog_write(
        level,
        component,
        message
    );
}

void klog_print(klog_level_t minimum_level)
{
    uint64_t flags = interrupt_save();
    size_t count = entry_count;
    size_t start = entry_start;
    interrupt_restore(flags);

    for (
        size_t index = 0;
        index < count;
        index++
    )
    {
        klog_entry_t entry;

        flags = interrupt_save();
        entry = entries[
            (start + index) % KLOG_CAPACITY
        ];
        interrupt_restore(flags);

        if (entry.level < minimum_level)
        {
            continue;
        }

        char line[KLOG_FORMAT_SIZE];

        ksnprintf(
            line,
            sizeof(line),
            "[%06llu] %s %s: %s",
            (unsigned long long)entry.sequence,
            klog_level_name(entry.level),
            entry.component,
            entry.message
        );

        terminal_write_line(line);
    }
}

void klog_clear(void)
{
    uint64_t flags = interrupt_save();

    entry_start = 0;
    entry_count = 0;

    interrupt_restore(flags);
}

size_t klog_entry_count(void)
{
    uint64_t flags = interrupt_save();
    size_t count = entry_count;
    interrupt_restore(flags);

    return count;
}
