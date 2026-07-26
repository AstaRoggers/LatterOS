#include "panic.h"

#include "graphics.h"
#include "klog.h"
#include "kstdio.h"
#include "serial.h"
#include "stacktrace.h"

#include <stddef.h>
#include <stdint.h>

#define PANIC_BACKGROUND 0x8B0000
#define PANIC_FOREGROUND 0xFFFFFF
#define PANIC_MUTED      0xFFD0D0

static void halt_cpu(void)
{
    __asm__ volatile("cli");

    for (;;)
    {
        __asm__ volatile("hlt");
    }
}

static uint64_t read_cr2(void)
{
    uint64_t value;

    __asm__ volatile(
        "movq %%cr2, %0"
        : "=r"(value)
    );

    return value;
}

static const char *exception_name(uint64_t vector)
{
    static const char *names[32] = {
        "Divide error",
        "Debug",
        "Non-maskable interrupt",
        "Breakpoint",
        "Overflow",
        "Bound range exceeded",
        "Invalid opcode",
        "Device not available",
        "Double fault",
        "Coprocessor segment overrun",
        "Invalid TSS",
        "Segment not present",
        "Stack-segment fault",
        "General protection fault",
        "Page fault",
        "Reserved",
        "x87 floating-point exception",
        "Alignment check",
        "Machine check",
        "SIMD floating-point exception",
        "Virtualization exception",
        "Control-protection exception",
        "Reserved",
        "Reserved",
        "Reserved",
        "Reserved",
        "Reserved",
        "Reserved",
        "Hypervisor injection exception",
        "VMM communication exception",
        "Security exception",
        "Reserved"
    };

    if (vector < 32)
    {
        return names[vector];
    }

    return "Unknown interrupt";
}

static void draw_line(
    uint32_t y,
    const char *label,
    const char *value
)
{
    draw_text(
        label,
        72,
        y,
        PANIC_MUTED
    );

    draw_text(
        value,
        208,
        y,
        PANIC_FOREGROUND
    );
}

static void panic_serial_report(
    const char *message,
    const cpu_context_t *context,
    uint64_t vector
)
{
    char line[256];

    serial_write_line("");
    serial_write_line("=== LATTEROS KERNEL PANIC ===");

    ksnprintf(
        line,
        sizeof(line),
        "Vector %llu (%s)",
        (unsigned long long)vector,
        exception_name(vector)
    );
    serial_write_line(line);

    ksnprintf(
        line,
        sizeof(line),
        "Reason: %s",
        message == NULL ? "Unknown" : message
    );
    serial_write_line(line);

    if (context != NULL)
    {
        ksnprintf(
            line,
            sizeof(line),
            "RIP=%016llX CS=%04llX RFLAGS=%016llX ERR=%016llX",
            (unsigned long long)context->rip,
            (unsigned long long)context->cs,
            (unsigned long long)context->rflags,
            (unsigned long long)context->error_code
        );
        serial_write_line(line);

        ksnprintf(
            line,
            sizeof(line),
            "RAX=%016llX RBX=%016llX RCX=%016llX RDX=%016llX",
            (unsigned long long)context->rax,
            (unsigned long long)context->rbx,
            (unsigned long long)context->rcx,
            (unsigned long long)context->rdx
        );
        serial_write_line(line);

        ksnprintf(
            line,
            sizeof(line),
            "RSI=%016llX RDI=%016llX RBP=%016llX",
            (unsigned long long)context->rsi,
            (unsigned long long)context->rdi,
            (unsigned long long)context->rbp
        );
        serial_write_line(line);

        if (vector == 14)
        {
            ksnprintf(
                line,
                sizeof(line),
                "CR2=%016llX",
                (unsigned long long)read_cr2()
            );
            serial_write_line(line);
        }

        uint64_t frames[STACKTRACE_MAX_FRAMES];
        size_t frame_count =
            stacktrace_collect_context(
                context,
                frames,
                STACKTRACE_MAX_FRAMES
            );

        serial_write_line("Stack trace:");

        for (
            size_t index = 0;
            index < frame_count;
            index++
        )
        {
            ksnprintf(
                line,
                sizeof(line),
                "  #%zu 0x%016llX",
                index,
                (unsigned long long)frames[index]
            );
            serial_write_line(line);
        }
    }
}

static void panic_screen(
    const char *message,
    const cpu_context_t *context,
    uint64_t vector
)
{
    char value[128];

    graphics_set_deferred(false);
    graphics_reset_clip();
    graphics_clear(PANIC_BACKGROUND);

    draw_text(
        "LATTEROS KERNEL PANIC",
        72,
        48,
        PANIC_FOREGROUND
    );

    draw_text(
        "A fatal kernel exception stopped the system.",
        72,
        76,
        PANIC_MUTED
    );

    ksnprintf(
        value,
        sizeof(value),
        "%llu - %s",
        (unsigned long long)vector,
        exception_name(vector)
    );
    draw_line(112, "Exception:", value);

    draw_line(
        136,
        "Reason:",
        message == NULL ? "Unknown" : message
    );

    if (context != NULL)
    {
        ksnprintf(
            value,
            sizeof(value),
            "0x%016llX",
            (unsigned long long)context->rip
        );
        draw_line(172, "RIP:", value);

        ksnprintf(
            value,
            sizeof(value),
            "0x%016llX",
            (unsigned long long)context->error_code
        );
        draw_line(196, "Error code:", value);

        ksnprintf(
            value,
            sizeof(value),
            "0x%016llX",
            (unsigned long long)context->rflags
        );
        draw_line(220, "RFLAGS:", value);

        ksnprintf(
            value,
            sizeof(value),
            "RAX=%016llX  RBX=%016llX",
            (unsigned long long)context->rax,
            (unsigned long long)context->rbx
        );
        draw_text(value, 72, 256, PANIC_FOREGROUND);

        ksnprintf(
            value,
            sizeof(value),
            "RCX=%016llX  RDX=%016llX",
            (unsigned long long)context->rcx,
            (unsigned long long)context->rdx
        );
        draw_text(value, 72, 280, PANIC_FOREGROUND);

        ksnprintf(
            value,
            sizeof(value),
            "RSI=%016llX  RDI=%016llX",
            (unsigned long long)context->rsi,
            (unsigned long long)context->rdi
        );
        draw_text(value, 72, 304, PANIC_FOREGROUND);

        ksnprintf(
            value,
            sizeof(value),
            "RBP=%016llX  CS=%04llX",
            (unsigned long long)context->rbp,
            (unsigned long long)context->cs
        );
        draw_text(value, 72, 328, PANIC_FOREGROUND);

        if (vector == 14)
        {
            ksnprintf(
                value,
                sizeof(value),
                "0x%016llX",
                (unsigned long long)read_cr2()
            );
            draw_line(364, "Fault address:", value);
        }
    }

    uint32_t message_y = 412;

    if (context != NULL)
    {
        uint64_t frames[5];
        size_t frame_count =
            stacktrace_collect_context(
                context,
                frames,
                5
            );

        draw_text(
            "Stack trace:",
            72,
            388,
            PANIC_MUTED
        );

        for (
            size_t index = 0;
            index < frame_count;
            index++
        )
        {
            ksnprintf(
                value,
                sizeof(value),
                "#%zu 0x%016llX",
                index,
                (unsigned long long)frames[index]
            );

            draw_text(
                value,
                88,
                412 + (uint32_t)index * 20,
                PANIC_FOREGROUND
            );
        }

        message_y =
            428 + (uint32_t)frame_count * 20;
    }

    draw_text(
        "A diagnostic copy was written to the serial log.",
        72,
        message_y,
        PANIC_MUTED
    );

    draw_text(
        "The CPU is halted. Reboot the machine to continue.",
        72,
        message_y + 24,
        PANIC_FOREGROUND
    );

    graphics_present();
}

void kernel_panic_context(
    const char *message,
    const cpu_context_t *context
)
{
    uint64_t vector =
        context == NULL ?
        UINT64_MAX :
        context->vector;

    klogf(
        KLOG_PANIC,
        "panic",
        "vector=%llu name=%s rip=0x%016llX reason=%s",
        (unsigned long long)vector,
        exception_name(vector),
        (unsigned long long)(
            context == NULL ? 0 : context->rip
        ),
        message == NULL ? "Unknown" : message
    );

    panic_serial_report(
        message,
        context,
        vector
    );

    panic_screen(
        message,
        context,
        vector
    );

    halt_cpu();
}

void kernel_panic(
    const char *message,
    uint64_t exception_number
)
{
    klogf(
        KLOG_PANIC,
        "panic",
        "vector=%llu reason=%s",
        (unsigned long long)exception_number,
        message == NULL ? "Unknown" : message
    );

    panic_serial_report(
        message,
        NULL,
        exception_number
    );

    panic_screen(
        message,
        NULL,
        exception_number
    );

    halt_cpu();
}
