#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limine.h>

#include "dhcp.h"
#include "dns.h"
#include "ethernet.h"
#include "gdt.h"
#include "graphics.h"
#include "heap.h"
#include "hhdm.h"
#include "icmp.h"
#include "idt.h"
#include "irq.h"
#include "ipv4.h"
#include "keyboard.h"
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

    serial_init();

    gdt_init();
    idt_init();

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
        hcf();
    }

    process_init();

    vfs_init();

    if (!ramfs_init())
    {
        hcf();
    }

    pci_init();
    storage_init();
    (void)usb_init();

    if (network_init())
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
    }

    terminal_init();

    pic_init();
    irq_init();

    bool lapic_ready =
        lapic_init();

    irq_set_lapic_enabled(
        lapic_ready
    );

    keyboard_init();

    irq_register_handler(
        1,
        keyboard_irq_handler
    );

    (void)mouse_init();
    speaker_init();

    timer_init(100);

    irq_enable();

    for (;;)
    {
        network_poll();

        __asm__ volatile(
            "hlt"
            :
            :
            : "memory"
        );
    }
}
