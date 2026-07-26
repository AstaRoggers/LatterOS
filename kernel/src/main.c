#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limine.h>

#include "acpi.h"
#include "autotest.h"
#include "dhcp.h"
#include "dns.h"
#include "ethernet.h"
#include "fat_fs.h"
#include "gdt.h"
#include "graphics.h"
#include "gui.h"
#include "heap.h"
#include "hhdm.h"
#include "icmp.h"
#include "idt.h"
#include "ioapic.h"
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
#include "security.h"
#include "speaker.h"
#include "smp.h"
#include "smp_scheduler.h"
#include "storage.h"
#include "tcp.h"
#include "terminal.h"
#include "timer.h"
#include "udp.h"
#include "usb.h"
#include "usb_mass_storage.h"
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

__attribute__((used, section(".limine_requests")))
static volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_mp_request mp_request = {
    .id = LIMINE_MP_REQUEST_ID,
    .revision = 0,
    .flags = LIMINE_MP_REQUEST_X86_64_X2APIC
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

#define BOOT_BACKGROUND 0x101820U
#define BOOT_PANEL      0x172330U
#define BOOT_ACCENT     0x39A0EDU
#define BOOT_TEXT       0xFFFFFFU
#define BOOT_MUTED      0x9CB3C9U

static uint32_t boot_bar_x;
static uint32_t boot_bar_y;
static uint32_t boot_bar_width;

static void boot_screen_begin(void)
{
    uint32_t width = graphics_width();
    uint32_t height = graphics_height();

    boot_bar_width = width >= 520 ? 360 : width - 80;
    boot_bar_x = (width - boot_bar_width) / 2;
    boot_bar_y = height / 2 + 34;

    graphics_clear(BOOT_BACKGROUND);

    draw_text(
        "LatterOS",
        width / 2 - 32,
        height / 2 - 54,
        BOOT_TEXT
    );

    draw_text(
        "Starting system",
        width / 2 - 60,
        height / 2 - 26,
        BOOT_MUTED
    );

    draw_rectangle(
        boot_bar_x - 2,
        boot_bar_y - 2,
        boot_bar_width + 4,
        14,
        BOOT_PANEL
    );
}

static void boot_screen_progress(
    uint32_t percentage,
    const char *stage
)
{
    if (percentage > 100)
    {
        percentage = 100;
    }

    uint32_t width = graphics_width();
    uint32_t height = graphics_height();
    uint32_t stage_y = height / 2 + 58;

    draw_rectangle(
        width / 2 - 140,
        stage_y,
        280,
        18,
        BOOT_BACKGROUND
    );

    if (stage != NULL)
    {
        draw_text(
            stage,
            width / 2 - 140,
            stage_y + 2,
            BOOT_MUTED
        );
    }

    draw_rectangle(
        boot_bar_x,
        boot_bar_y,
        boot_bar_width,
        10,
        BOOT_PANEL
    );

    draw_rectangle(
        boot_bar_x,
        boot_bar_y,
        (boot_bar_width * percentage) / 100,
        10,
        BOOT_ACCENT
    );
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

    boot_screen_begin();
    boot_screen_progress(8, "Initializing kernel");

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

    bool acpi_ready =
        acpi_init(
            rsdp_request.response != NULL ?
                rsdp_request.response->address :
                NULL
        );

    klogf(
        acpi_ready ? KLOG_INFO : KLOG_WARNING,
        "acpi",
        "tables=%s root=%s cpus=%u ioapics=%u",
        acpi_ready ? "ready" : "unavailable",
        acpi_ready ? acpi_root_table_name() : "none",
        (unsigned int)acpi_cpu_count(),
        (unsigned int)acpi_ioapic_count()
    );

    boot_screen_progress(22, "Detecting hardware");

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

    boot_screen_progress(38, "Preparing memory");

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

    boot_screen_progress(52, "Mounting storage");

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

    if (vfs_open("/media") == NULL)
    {
        if (!vfs_make_directory("/media"))
        {
            klog_write(
                KLOG_WARNING,
                "filesystem",
                "Unable to create /media mount directory"
            );
        }
    }

    security_init();

    klogf(
        KLOG_INFO,
        "security",
        "session user=%s uid=%u",
        security_current_username(),
        (unsigned int)security_session_uid()
    );

    boot_screen_progress(66, "Starting USB devices");

    bool usb_ready = usb_init();

    bool usb_storage_ready =
        usb_ready &&
        usb_mass_storage_init();

    bool fat_ready =
        usb_storage_ready &&
        fat_fs_mount_first_usb();

    if (fat_ready)
    {
        klog_write(
            KLOG_INFO,
            "filesystem",
            "USB FAT filesystem mounted at /media/usb"
        );
    }
    else if (usb_storage_ready)
    {
        klog_write(
            KLOG_WARNING,
            "filesystem",
            "USB storage found without a mountable FAT volume"
        );
    }

    klogf(
        usb_ready ? KLOG_INFO : KLOG_WARNING,
        "usb",
        "controller=%s connected=%u enumerated=%u keyboard=%s mouse=%s storage=%u",
        usb_ready ? "ready" : "not detected",
        (unsigned int)usb_connected_port_count(),
        (unsigned int)usb_device_count(),
        usb_keyboard_ready() ? "USB" : "PS/2",
        usb_mouse_ready() ? "USB" : "PS/2",
        (unsigned int)(
            usb_storage_ready ?
                usb_mass_storage_count() :
                0
        )
    );

    boot_screen_progress(76, "Starting network");

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

    boot_screen_progress(84, "Configuring interrupts");

    terminal_init();

    pic_init();
    irq_init();

    bool lapic_ready =
        lapic_init();

    bool ioapic_ready = false;

    if (lapic_ready && acpi_ready)
    {
        ioapic_ready =
            ioapic_init(lapic_id());
    }

    irq_set_lapic_enabled(
        lapic_ready
    );

    irq_set_ioapic_enabled(
        ioapic_ready
    );

    if (ioapic_ready)
    {
        pic_mask_all();
        lapic_set_legacy_pic(false);
    }
    else if (lapic_ready)
    {
        lapic_set_legacy_pic(true);
    }

    klogf(
        lapic_ready ? KLOG_INFO : KLOG_WARNING,
        "interrupts",
        "x2APIC=%s controller=%s bsp-apic-id=%u",
        lapic_ready ? "enabled" : "unavailable",
        irq_controller_name(),
        (unsigned int)lapic_id()
    );

    keyboard_init();

    irq_register_handler(
        1,
        keyboard_irq_handler
    );

    (void)mouse_init();
    gui_init();
    speaker_init();

    timer_init(1000);

    klog_write(
        KLOG_INFO,
        "timer",
        "PIT configured at 1000 Hz"
    );

    irq_enable();

    boot_screen_progress(92, "Starting processor cores");

    smp_scheduler_init();

    bool smp_ready =
        smp_init(
            mp_request.response
        );

    klogf(
        smp_ready ? KLOG_INFO : KLOG_WARNING,
        "smp",
        "discovered=%u online=%u mode=%s ap-workers=%u",
        (unsigned int)smp_cpu_count(),
        (unsigned int)smp_online_count(),
        smp_uses_x2apic() ? "x2APIC" : "unavailable",
        (unsigned int)smp_scheduler_worker_count()
    );

#ifdef LATTEROS_AUTOTEST
    autotest_start();
#else
    boot_screen_progress(100, "Starting desktop");
    gui_request_start();

    klog_write(
        KLOG_INFO,
        "desktop",
        "Desktop startup requested"
    );
#endif

    for (;;)
    {
        usb_poll();
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
