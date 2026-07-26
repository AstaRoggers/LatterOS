#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limine.h>

#include "autotest.h"
#include "dhcp.h"
#include "dns.h"
#include "ethernet.h"
#include "gdt.h"
#include "graphics.h"
#include "gui.h"
#include "heap.h"
#include "hhdm.h"
#include "icmp.h"
#include "idt.h"
#include "irq.h"
#include "ipv4.h"
#include "keyboard.h"
#include "klog.h"
#include "latteros_fs.h"
#include "lapic.h"
#include "mouse.h"
#include "network.h"
#include "page_allocator.h"
#include "pci.h"
#include "physical_memory.h"
#include "pic.h"
#include "process.h"
#include "ramfs.h"
#include "serial.h"
#include "speaker.h"
#include "storage.h"
#include "tcp.h"
#include "terminal.h"
#include "timer.h"
#include "udp.h"
#include "usb.h"
#include "vfs.h"

__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_revision[] =
    LIMINE_BASE_REVISION(6);

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memory_map_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t limine_requests_start_marker[] =
    LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t limine_requests_end_marker[] =
    LIMINE_REQUESTS_END_MARKER;

static void hcf(void)
{
    __asm__ volatile("cli");

    for (;;)
    {
        __asm__ volatile("hlt");
    }
}

static bool test_heap(void)
{
    uint64_t *numbers =
        kmalloc(
            32 * sizeof(uint64_t)
        );

    char *text =
        kmalloc(128);

    if (
        numbers == NULL ||
        text == NULL
    )
    {
        return false;
    }

    for (
        uint64_t i = 0;
        i < 32;
        i++
    )
    {
        numbers[i] =
            0x1000000000000000ULL + i;
    }

    for (
        uint64_t i = 0;
        i < 128;
        i++
    )
    {
        text[i] =
            (char)(i & 0x7F);
    }

    for (
        uint64_t i = 0;
        i < 32;
        i++
    )
    {
        if (
            numbers[i] !=
            0x1000000000000000ULL + i
        )
        {
            return false;
        }
    }

    for (
        uint64_t i = 0;
        i < 128;
        i++
    )
    {
        if (
            text[i] !=
            (char)(i & 0x7F)
        )
        {
            return false;
        }
    }

    kfree(numbers);
    kfree(text);

    void *reused =
        kmalloc(256);

    if (reused == NULL)
    {
        return false;
    }

    kfree(reused);

    return true;
}

void kmain(void)
{
    if (
        !LIMINE_BASE_REVISION_SUPPORTED(
            limine_base_revision
        )
    )
    {
        hcf();
    }

    if (
        framebuffer_request.response == NULL ||
        framebuffer_request.response
            ->framebuffer_count < 1
    )
    {
        hcf();
    }

    if (
        memory_map_request.response == NULL ||
        hhdm_request.response == NULL
    )
    {
        hcf();
    }

    struct limine_framebuffer *framebuffer =
        framebuffer_request.response
            ->framebuffers[0];

    graphics_init(
        (uint32_t *)framebuffer->address,
        framebuffer->pitch,
        framebuffer->width,
        framebuffer->height
    );

    graphics_clear(0x0066CC);

    bool serial_ready = serial_init();

    klog_init();
    klogf(
        KLOG_INFO,
        "boot",
        "LatterOS starting; framebuffer=%ux%u pitch=%llu serial=%s",
        (unsigned int)framebuffer->width,
        (unsigned int)framebuffer->height,
        (unsigned long long)framebuffer->pitch,
        serial_ready ? "ready" : "unavailable"
    );

    gdt_init();
    idt_init();

    klog_write(
        KLOG_INFO,
        "cpu",
        "GDT and IDT initialized"
    );

    hhdm_init(
        hhdm_request.response
    );

    physical_memory_init(
        memory_map_request.response
    );

    page_allocator_init();
    heap_init();

    if (!test_heap())
    {
        klog_write(
            KLOG_PANIC,
            "memory",
            "Heap self-test failed"
        );

        hcf();
    }

    klogf(
        KLOG_INFO,
        "memory",
        "usable=%llu KiB pages=%llu free=%llu",
        (unsigned long long)(
            physical_memory_usable_bytes() / 1024ULL
        ),
        (unsigned long long)
            physical_memory_usable_pages(),
        (unsigned long long)free_page_count()
    );

    process_init();

    klog_write(
        KLOG_INFO,
        "process",
        "Process scheduler initialized"
    );

    pci_init();
    storage_init();

    klogf(
        KLOG_INFO,
        "pci",
        "devices=%u storage=%s",
        (unsigned int)pci_device_count(),
        storage_controller_found() ?
            "detected" : "not detected"
    );

    vfs_init();

    if (!ramfs_init())
    {
        klog_write(
            KLOG_PANIC,
            "filesystem",
            "RAM filesystem initialization failed"
        );

        hcf();
    }

    if (!latteros_fs_init())
    {
        klog_write(
            KLOG_WARNING,
            "filesystem",
            "Persistent /home unavailable; using RAM fallback"
        );

        if (
            !vfs_make_directory("/home") ||
            !vfs_write_text(
                "/home/welcome.txt",
                "Persistent storage unavailable; using RAM home.\n"
            )
        )
        {
            klog_write(
                KLOG_PANIC,
                "filesystem",
                "Unable to create fallback /home"
            );

            hcf();
        }
    }
    else
    {
        klogf(
            KLOG_INFO,
            "filesystem",
            "LatterOS filesystem mounted; entries=%u used=%llu bytes",
            (unsigned int)latteros_fs_entry_count(),
            (unsigned long long)latteros_fs_used_bytes()
        );
    }

    bool usb_ready = usb_init();

    klogf(
        usb_ready ? KLOG_INFO : KLOG_WARNING,
        "usb",
        "controller=%s",
        usb_ready ? "ready" : "not detected"
    );

    bool network_ready = network_init();

    if (network_ready)
    {
        ethernet_init();

        ipv4_init(
            IPV4_ADDRESS(10, 0, 2, 15),
            IPV4_ADDRESS(255, 255, 255, 0),
            IPV4_ADDRESS(10, 0, 2, 2)
        );

        udp_init();
        tcp_init();
        dhcp_init();

        dns_init(
            IPV4_ADDRESS(10, 0, 2, 3)
        );

        icmp_init();

        klog_write(
            KLOG_INFO,
            "network",
            "RTL8139 and IPv4 stack initialized"
        );
    }
    else
    {
        klog_write(
            KLOG_WARNING,
            "network",
            "Network card not available"
        );
    }

    terminal_init();

    pic_init();
    irq_init();

    bool lapic_ready =
        lapic_init();

    irq_set_lapic_enabled(
        lapic_ready
    );

    klogf(
        lapic_ready ? KLOG_INFO : KLOG_WARNING,
        "interrupts",
        "x2APIC=%s",
        lapic_ready ? "enabled" : "unavailable"
    );

    keyboard_init();

    irq_register_handler(
        1,
        keyboard_irq_handler
    );

    (void)mouse_init();
    gui_init();
    speaker_init();

    timer_init(100);

    klog_write(
        KLOG_INFO,
        "timer",
        "PIT configured at 100 Hz"
    );

    irq_enable();

#ifdef LATTEROS_AUTOTEST
    autotest_start();
#else
    gui_request_start();

    klog_write(
        KLOG_INFO,
        "desktop",
        "Desktop startup requested"
    );
#endif

    for (;;)
    {
        network_poll();

#ifdef LATTEROS_AUTOTEST
        autotest_update();
#else
        gui_update();
#endif

        __asm__ volatile(
            "hlt"
            :
            :
            : "memory"
        );
    }
}
