#include "power.h"

#include "io.h"
#include "irq.h"
#include "klog.h"
#include "latteros_fs.h"

#include <stdint.h>

#define PS2_STATUS_PORT  0x64
#define PS2_COMMAND_PORT 0x64
#define PS2_INPUT_FULL   0x02
#define PS2_CPU_RESET    0xFE
#define REBOOT_TIMEOUT   1000000U

static void halt_forever(void)
{
    irq_disable();

    for (;;)
    {
        __asm__ volatile("hlt");
    }
}

static void triple_fault(void)
{
    struct __attribute__((packed))
    {
        uint16_t limit;
        uint64_t base;
    } empty_idt = {
        .limit = 0,
        .base = 0
    };

    __asm__ volatile(
        "lidt %0\n"
        "int3"
        :
        : "m"(empty_idt)
        : "memory"
    );

    halt_forever();
}

void power_reboot(void)
{
    klog_write(
        KLOG_INFO,
        "power",
        "Reboot requested"
    );

    (void)latteros_fs_sync();
    irq_disable();

    for (
        uint32_t timeout = 0;
        timeout < REBOOT_TIMEOUT;
        timeout++
    )
    {
        if (
            !(inb(PS2_STATUS_PORT) &
            PS2_INPUT_FULL)
        )
        {
            outb(
                PS2_COMMAND_PORT,
                PS2_CPU_RESET
            );

            break;
        }

        __asm__ volatile("pause");
    }

    triple_fault();
}

void power_shutdown(void)
{
    klog_write(
        KLOG_INFO,
        "power",
        "Shutdown requested"
    );

    (void)latteros_fs_sync();
    irq_disable();

    /* QEMU/Bochs ACPI power-management ports. */
    outw(0x0604, 0x2000);
    outw(0xB004, 0x2000);

    /* VirtualBox-compatible fallback. */
    outw(0x4004, 0x3400);

    halt_forever();
}
