#include "gui.h"
#include "graphics.h"
#include "heap.h"
#include "shell.h"

#include "acpi.h"
#include "ahci.h"
#include "arp.h"
#include "block_device.h"
#include "compositor.h"
#include "dhcp.h"
#include "dns.h"
#include "fat_fs.h"
#include "kstdio.h"
#include "klog.h"
#include "kstdlib.h"
#include "kstring.h"
#include "icmp.h"
#include "ioapic.h"
#include "irq.h"
#include "ipv4.h"
#include "lapic.h"
#include "latteros_fs.h"
#include "mouse.h"
#include "network.h"
#include "nvme.h"
#include "page_allocator.h"
#include "physical_memory.h"
#include "pci.h"
#include "process.h"
#include "partition.h"
#include "power.h"
#include "rtc.h"
#include "serial.h"
#include "security.h"
#include "selftest.h"
#include "stacktrace.h"
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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DISK_TEST_LBA 2048ULL
#define DISK_SECTOR_BUFFER_SIZE 512

typedef void (*command_function_t)(
    const char *arguments
);

typedef struct
{
    const char *name;
    const char *description;
    command_function_t function;
} shell_command_t;

static uint8_t disk_write_buffer[
    DISK_SECTOR_BUFFER_SIZE
];

static uint8_t disk_read_buffer[
    DISK_SECTOR_BUFFER_SIZE
];

static bool strings_equal(
    const char *first,
    const char *second
)
{
    uint32_t index = 0;

    while (
        first[index] != '\0' &&
        second[index] != '\0'
    )
    {
        if (first[index] != second[index])
        {
            return false;
        }

        index++;
    }

    return first[index] == second[index];
}

static bool character_is_space(
    char character
)
{
    return (
        character == ' ' ||
        character == '\t'
    );
}

static bool split_first_argument(
    const char *arguments,
    char *first,
    size_t first_capacity,
    const char **remaining
)
{
    if (
        arguments == NULL ||
        first == NULL ||
        first_capacity < 2 ||
        remaining == NULL
    )
    {
        return false;
    }

    size_t input = 0;

    while (character_is_space(arguments[input]))
    {
        input++;
    }

    bool quoted = arguments[input] == '"';

    if (quoted)
    {
        input++;
    }

    size_t output = 0;

    while (arguments[input] != '\0')
    {
        if (
            (quoted && arguments[input] == '"') ||
            (!quoted && character_is_space(arguments[input]))
        )
        {
            break;
        }

        if (output + 1 >= first_capacity)
        {
            return false;
        }

        first[output++] = arguments[input++];
    }

    if (quoted)
    {
        if (arguments[input] != '"')
        {
            return false;
        }

        input++;
    }

    first[output] = '\0';

    while (character_is_space(arguments[input]))
    {
        input++;
    }

    *remaining = &arguments[input];
    return output > 0;
}

static bool parse_single_argument(
    const char *arguments,
    char *value,
    size_t capacity
)
{
    const char *remaining;

    if (!split_first_argument(
            arguments,
            value,
            capacity,
            &remaining
        ))
    {
        return false;
    }

    return remaining[0] == '\0';
}

static bool parse_uint64(
    const char *text,
    uint64_t *value
)
{
    if (
        text == NULL ||
        value == NULL ||
        text[0] == '\0'
    )
    {
        return false;
    }

    uint64_t result = 0;
    uint32_t index = 0;

    while (text[index] != '\0')
    {
        char character = text[index];

        if (
            character < '0' ||
            character > '9'
        )
        {
            return false;
        }

        uint64_t digit =
            (uint64_t)(character - '0');

        if (
            result >
            (UINT64_MAX - digit) / 10
        )
        {
            return false;
        }

        result = result * 10 + digit;
        index++;
    }

    *value = result;
    return true;
}

static bool parse_octal_mode(
    const char *text,
    uint16_t *mode
)
{
    if (
        text == NULL ||
        mode == NULL ||
        text[0] == '\0'
    )
    {
        return false;
    }

    uint16_t value = 0;
    uint32_t index = 0;

    while (text[index] != '\0')
    {
        if (
            text[index] < '0' ||
            text[index] > '7'
        )
        {
            return false;
        }

        value = (uint16_t)(
            value * 8U +
            (uint16_t)(text[index] - '0')
        );

        if (value > 0777U)
        {
            return false;
        }

        index++;
    }

    *mode = value;
    return true;
}

static void mode_to_string(
    uint16_t mode,
    char text[4]
)
{
    text[0] = (char)('0' + ((mode >> 6) & 7U));
    text[1] = (char)('0' + ((mode >> 3) & 7U));
    text[2] = (char)('0' + (mode & 7U));
    text[3] = '\0';
}

static void uint64_to_string(
    uint64_t value,
    char *buffer
)
{
    char temporary[21];
    uint32_t length = 0;

    if (value == 0)
    {
        buffer[0] = '0';
        buffer[1] = '\0';
        return;
    }

    while (value > 0)
    {
        temporary[length] =
            (char)(
                '0' +
                value % 10
            );

        length++;
        value /= 10;
    }

    for (
        uint32_t index = 0;
        index < length;
        index++
    )
    {
        buffer[index] =
            temporary[
                length -
                index -
                1
            ];
    }

    buffer[length] = '\0';
}

static void terminal_write_hex_byte(
    uint8_t value
)
{
    static const char hexadecimal[] =
        "0123456789ABCDEF";

    char text[3];

    text[0] =
        hexadecimal[(value >> 4) & 0x0F];

    text[1] =
        hexadecimal[value & 0x0F];

    text[2] = '\0';

    terminal_write(text);
}

static void command_help(
    const char *arguments
);

static void command_clear(
    const char *arguments
);

static void command_version(
    const char *arguments
);

static void command_echo(
    const char *arguments
);

static void command_ticks(
    const char *arguments
);

static void command_gfxstats(
    const char *arguments
);

static void command_lspci(
    const char *arguments
);

static void command_acpi(
    const char *arguments
);

static void command_irqinfo(
    const char *arguments
);

static void command_smp(
    const char *arguments
);

static void command_smpwork(
    const char *arguments
);

static void command_smpjobs(
    const char *arguments
);

static void command_storage(
    const char *arguments
);

static void command_ahci(
    const char *arguments
);

static void command_ahcitest(
    const char *arguments
);

static void command_nvme(
    const char *arguments
);

static void command_nvmetest(
    const char *arguments
);

static void command_partitions(
    const char *arguments
);

static void command_disks(
    const char *arguments
);

static void command_diskread(
    const char *arguments
);

static void command_disktest(
    const char *arguments
);

static void command_ls(
    const char *arguments
);

static void command_cd(
    const char *arguments
);

static void command_pwd(
    const char *arguments
);

static void command_cat(
    const char *arguments
);

static void command_mkdir(
    const char *arguments
);

static void command_write(
    const char *arguments
);

static void command_rm(
    const char *arguments
);

static void command_rename(
    const char *arguments
);

static void command_cp(
    const char *arguments
);

static void command_mv(
    const char *arguments
);

static void command_ps(
    const char *arguments
);

static void command_spawn(
    const char *arguments
);

static void command_run(
    const char *arguments
);

static void command_kill(
    const char *arguments
);

static void command_libtest(
    const char *arguments
);

static void command_apitest(
    const char *arguments
);

static void command_securitytest(
    const char *arguments
);

static void command_whoami(
    const char *arguments
);

static void command_id(
    const char *arguments
);

static void command_users(
    const char *arguments
);

static void command_login(
    const char *arguments
);

static void command_logout(
    const char *arguments
);

static void command_caps(
    const char *arguments
);

static void command_stat(
    const char *arguments
);

static void command_chmod(
    const char *arguments
);

static void command_chown(
    const char *arguments
);

static void command_permtest(
    const char *arguments
);

static void command_mouse(
    const char *arguments
);

static void command_beep(
    const char *arguments
);

static void command_date(
    const char *arguments
);

static void command_serialtest(
    const char *arguments
);

static void command_usb(
    const char *arguments
);

static void command_usbms(
    const char *arguments
);

static void command_fatinfo(
    const char *arguments
);

static void command_mountusb(
    const char *arguments
);

static void command_mountsata(
    const char *arguments
);

static void command_mountnvme(
    const char *arguments
);

static void command_umountusb(
    const char *arguments
);

static void command_fatrwtest(
    const char *arguments
);

static void command_net(
    const char *arguments
);

static void command_netsend(
    const char *arguments
);

static void command_ifconfig(
    const char *arguments
);

static void command_arp(
    const char *arguments
);

static void command_ping(
    const char *arguments
);

static void command_udp(
    const char *arguments
);

static void command_dhcp(
    const char *arguments
);

static void command_dns(
    const char *arguments
);

static void command_tcp(
    const char *arguments
);

static void command_http(
    const char *arguments
);

static void command_fsinfo(
    const char *arguments
);

static void command_sync(
    const char *arguments
);

static void command_dmesg(
    const char *arguments
);

static void command_logtest(
    const char *arguments
);

static void command_sysinfo(
    const char *arguments
);

static void command_trace(
    const char *arguments
);

static void command_meminfo(
    const char *arguments
);

static void command_memtest(
    const char *arguments
);

static void command_guardtest(
    const char *arguments
);

static void command_schedtest(
    const char *arguments
);

static void command_schedstatus(
    const char *arguments
);

static void command_fstest(
    const char *arguments
);

static void command_nettest(
    const char *arguments
);

static void command_reboot(
    const char *arguments
);

static void command_gui(
    const char *arguments
);

static void command_shutdown(
    const char *arguments
);

static void command_panic(
    const char *arguments
);

static const shell_command_t commands[] = {
    {
        "help",
        "Show available commands",
        command_help
    },
    {
        "clear",
        "Clear the screen",
        command_clear
    },
    {
        "version",
        "Show OS version",
        command_version
    },
    {
        "echo",
        "Print text",
        command_echo
    },
    {
        "ticks",
        "Show timer tick count",
        command_ticks
    },
    {
        "gfxstats",
        "Show compositor performance: gfxstats [reset]",
        command_gfxstats
    },
    {
        "lspci",
        "List detected PCI devices",
        command_lspci
    },
    {
        "acpi",
        "Show ACPI CPU and interrupt tables",
        command_acpi
    },
    {
        "irqinfo",
        "Show active interrupt-controller routing",
        command_irqinfo
    },
    {
        "smp",
        "Show multiprocessor startup and per-CPU state",
        command_smp
    },
    {
        "smpwork",
        "Run parallel AP work: smpwork [iterations]",
        command_smpwork
    },
    {
        "smpjobs",
        "Show AP work-queue status",
        command_smpjobs
    },
    {
        "storage",
        "Show storage controller",
        command_storage
    },
    {
        "ahci",
        "Show AHCI controller and SATA ports",
        command_ahci
    },
    {
        "ahcitest",
        "Test AHCI DMA read/write and restore",
        command_ahcitest
    },
    {
        "nvme",
        "Show NVMe controller and namespaces",
        command_nvme
    },
    {
        "nvmetest",
        "Test NVMe read/write/flush and restore",
        command_nvmetest
    },
    {
        "partitions",
        "List detected MBR and GPT partitions",
        command_partitions
    },
    {
        "disks",
        "List block devices",
        command_disks
    },
    {
        "diskread",
        "Read sector: diskread [DISK] LBA",
        command_diskread
    },
    {
        "disktest",
        "Test disk write and read",
        command_disktest
    },
    {
        "ls",
        "List files and directories",
        command_ls
    },
    {
        "cd",
        "Change directory",
        command_cd
    },
    {
        "pwd",
        "Show current directory",
        command_pwd
    },
    {
        "cat",
        "Read a text file",
        command_cat
    },
    {
        "mkdir",
        "Create a directory",
        command_mkdir
    },
    {
        "write",
        "Write text: write PATH TEXT",
        command_write
    },
    {
        "rm",
        "Delete file/directory: rm PATH",
        command_rm
    },
    {
        "rename",
        "Rename: rename PATH NEW_NAME",
        command_rename
    },
    {
        "cp",
        "Copy: cp SOURCE DESTINATION",
        command_cp
    },
    {
        "mv",
        "Move: mv SOURCE DESTINATION",
        command_mv
    },
    {
        "ps",
        "List processes",
        command_ps
    },
    {
        "spawn",
        "Create a kernel worker thread",
        command_spawn
    },
    {
        "run",
        "Start a user program: run PATH",
        command_run
    },
    {
        "kill",
        "Terminate a process: kill PID",
        command_kill
    },
    {
        "libtest",
        "Test kernel string and formatting library",
        command_libtest
    },
    {
        "apitest",
        "Test user runtime file and process APIs",
        command_apitest
    },
    {
        "securitytest",
        "Test user isolation and syscall validation",
        command_securitytest
    },
    {
        "whoami",
        "Show the current login name",
        command_whoami
    },
    {
        "id",
        "Show session UID, GID, and capabilities",
        command_id
    },
    {
        "users",
        "List built-in user accounts",
        command_users
    },
    {
        "login",
        "Switch account: login USER PASSWORD",
        command_login
    },
    {
        "logout",
        "Switch to the guest session",
        command_logout
    },
    {
        "caps",
        "Show current session capabilities",
        command_caps
    },
    {
        "stat",
        "Show ownership and mode: stat PATH",
        command_stat
    },
    {
        "chmod",
        "Change mode: chmod MODE PATH",
        command_chmod
    },
    {
        "chown",
        "Change owner: chown USER PATH",
        command_chown
    },
    {
        "permtest",
        "Test users, ownership, modes, and capabilities",
        command_permtest
    },
    {
        "mouse",
        "Show PS/2 mouse state",
        command_mouse
    },
    {
        "beep",
        "PC speaker: beep [Hz] [ms]",
        command_beep
    },
    {
        "date",
        "Show the RTC date and time",
        command_date
    },
    {
        "serialtest",
        "Test the COM1 serial driver",
        command_serialtest
    },
    {
        "usb",
        "Show USB host controller status",
        command_usb
    },
    {
        "usbms",
        "Show USB mass-storage devices",
        command_usbms
    },
    {
        "fatinfo",
        "Show mounted FAT filesystem status",
        command_fatinfo
    },
    {
        "mountusb",
        "Switch to the first USB FAT volume",
        command_mountusb
    },
    {
        "mountsata",
        "Switch to the first AHCI FAT partition",
        command_mountsata
    },
    {
        "mountnvme",
        "Switch to the first NVMe FAT partition",
        command_mountnvme
    },
    {
        "umountusb",
        "Synchronize and safely unmount USB FAT",
        command_umountusb
    },
    {
        "fatrwtest",
        "Test FAT create, resize, rename, move, and delete",
        command_fatrwtest
    },
    {
        "net",
        "Show network card status",
        command_net
    },
    {
        "netsend",
        "Transmit a raw Ethernet test frame",
        command_netsend
    },
    {
        "ifconfig",
        "Show IPv4 configuration",
        command_ifconfig
    },
    {
        "arp",
        "Resolve or show ARP entries",
        command_arp
    },
    {
        "ping",
        "Send ICMP echo: ping IPv4",
        command_ping
    },
    {
        "udp",
        "Show UDP transport status",
        command_udp
    },
    {
        "dhcp",
        "Request and show a DHCP lease",
        command_dhcp
    },
    {
        "dns",
        "Resolve an A record: dns NAME",
        command_dns
    },
    {
        "tcp",
        "Test TCP connection: tcp HOST [PORT]",
        command_tcp
    },
    {
        "http",
        "Fetch HTTP page: http HOST [PATH]",
        command_http
    },
    {
        "fsinfo",
        "Show persistent filesystem status",
        command_fsinfo
    },
    {
        "sync",
        "Flush persistent filesystem metadata",
        command_sync
    },
    {
        "dmesg",
        "Show kernel log: dmesg [LEVEL|clear]",
        command_dmesg
    },
    {
        "logtest",
        "Write test messages to the kernel log",
        command_logtest
    },
    {
        "sysinfo",
        "Show system health and resource status",
        command_sysinfo
    },
    {
        "trace",
        "Print the current kernel stack trace",
        command_trace
    },
    {
        "meminfo",
        "Show heap tracking: meminfo [leaks]",
        command_meminfo
    },
    {
        "memtest",
        "Run heap tracking and integrity tests",
        command_memtest
    },
    {
        "guardtest",
        "Validate process stack guard pages",
        command_guardtest
    },
    {
        "schedtest",
        "Start scheduler stress workers",
        command_schedtest
    },
    {
        "schedstatus",
        "Show scheduler stress progress",
        command_schedstatus
    },
    {
        "fstest",
        "Run persistent filesystem tests",
        command_fstest
    },
    {
        "nettest",
        "Run ARP, ICMP, DNS, UDP, and TCP tests",
        command_nettest
    },
    {
        "reboot",
        "Restart LatterOS",
        command_reboot
    },
    {
        "gui",
        "Launch the graphical desktop",
        command_gui
    },
    {
        "shutdown",
        "Shut down LatterOS",
        command_shutdown
    },
    {
        "panic",
        "Test CPU exception handling",
        command_panic
    }
};

#define COMMAND_COUNT \
    (sizeof(commands) / sizeof(commands[0]))

static void command_help(
    const char *arguments
)
{
    (void)arguments;

    terminal_write_line(
        "Available commands:"
    );

    for (
        uint32_t index = 0;
        index < COMMAND_COUNT;
        index++
    )
    {
        terminal_write(commands[index].name);
        terminal_write(" - ");

        terminal_write_line(
            commands[index].description
        );
    }
}

static void command_clear(
    const char *arguments
)
{
    (void)arguments;
    terminal_clear();
}

static void command_version(
    const char *arguments
)
{
    (void)arguments;

    terminal_write_line(
        "LatterOS version 0.1"
    );
}

static void command_echo(
    const char *arguments
)
{
    terminal_write_line(arguments);
}

static void command_ticks(
    const char *arguments
)
{
    (void)arguments;

    char text[21];

    uint64_to_string(
        timer_ticks(),
        text
    );

    terminal_write("Timer ticks: ");
    terminal_write_line(text);
}

static void command_gfxstats(
    const char *arguments
)
{
    if (strings_equal(arguments, "reset"))
    {
        compositor_reset_statistics();
        graphics_reset_motion_statistics();
        terminal_write_line(
            "Graphics statistics reset"
        );
        return;
    }

    char text[21];

    terminal_write("Timer frequency: ");
    uint64_to_string(timer_frequency(), text);
    terminal_write(text);
    terminal_write_line(" Hz");

    terminal_write("Idle-inclusive FPS: ");
    uint64_to_string(
        compositor_average_fps(),
        text
    );
    terminal_write_line(text);

    terminal_write("Recent active FPS: ");
    uint64_to_string(
        compositor_recent_active_fps(),
        text
    );
    terminal_write_line(text);

    terminal_write("Average render time: ");
    uint64_to_string(
        compositor_average_render_ms(),
        text
    );
    terminal_write(text);
    terminal_write_line(" ms");

    terminal_write("Average present time: ");
    uint64_to_string(
        compositor_average_present_ms(),
        text
    );
    terminal_write(text);
    terminal_write_line(" ms");

    uint64_t frames =
        compositor_frame_count();

    terminal_write("Rendered frames: ");
    uint64_to_string(frames, text);
    terminal_write_line(text);

    terminal_write("Dirty rectangles: ");
    uint64_to_string(
        compositor_rectangle_count(),
        text
    );
    terminal_write_line(text);

    uint64_t pixels =
        compositor_pixel_count();

    terminal_write("Presented pixels: ");
    uint64_to_string(pixels, text);
    terminal_write_line(text);

    terminal_write("Average pixels/frame: ");
    uint64_to_string(
        frames == 0 ? 0 : pixels / frames,
        text
    );
    terminal_write_line(text);

    terminal_write("Cursor fast-path updates: ");
    uint64_to_string(
        graphics_cursor_update_count(),
        text
    );
    terminal_write_line(text);

    terminal_write("Cached drag blits: ");
    uint64_to_string(
        graphics_surface_blit_count(),
        text
    );
    terminal_write_line(text);

    terminal_write("Framebuffer: ");
    uint64_to_string(graphics_width(), text);
    terminal_write(text);
    terminal_write("x");
    uint64_to_string(graphics_height(), text);
    terminal_write_line(text);
}

static void command_lspci(
    const char *arguments
)
{
    (void)arguments;
    pci_print_devices();
}

static void command_acpi(
    const char *arguments
)
{
    (void)arguments;

    if (!acpi_is_available())
    {
        terminal_write_line(
            "ACPI MADT is not available"
        );
        return;
    }

    kprintf(
        "ACPI: root=%s OEM=%s LAPIC=0x%llX\n",
        acpi_root_table_name(),
        acpi_oem_id(),
        (unsigned long long)
            acpi_local_apic_address()
    );

    kprintf(
        "CPUs: %u  I/O APICs: %u  IRQ overrides: %u\n",
        (unsigned int)acpi_cpu_count(),
        (unsigned int)acpi_ioapic_count(),
        (unsigned int)acpi_iso_count()
    );

    for (
        uint32_t index = 0;
        index < acpi_cpu_count();
        index++
    )
    {
        const acpi_cpu_info_t *cpu =
            acpi_cpu(index);

        if (cpu == NULL)
        {
            continue;
        }

        kprintf(
            "CPU %u: APIC ID=%u UID=%u %s %s\n",
            (unsigned int)index,
            (unsigned int)cpu->apic_id,
            (unsigned int)cpu->acpi_uid,
            cpu->enabled ? "enabled" : "disabled",
            cpu->x2apic ? "x2APIC" : "xAPIC"
        );
    }

    for (
        uint32_t index = 0;
        index < acpi_ioapic_count();
        index++
    )
    {
        const acpi_ioapic_info_t *controller =
            acpi_ioapic(index);

        if (controller == NULL)
        {
            continue;
        }

        kprintf(
            "I/O APIC %u: ID=%u address=0x%08X GSI base=%u\n",
            (unsigned int)index,
            (unsigned int)controller->id,
            (unsigned int)controller->address,
            (unsigned int)controller->gsi_base
        );
    }

    for (
        uint32_t index = 0;
        index < acpi_iso_count();
        index++
    )
    {
        const acpi_iso_info_t *override =
            acpi_iso(index);

        if (override == NULL)
        {
            continue;
        }

        kprintf(
            "IRQ override: IRQ%u -> GSI%u %s %s\n",
            (unsigned int)override->source_irq,
            (unsigned int)override->gsi,
            override->active_low ?
                "active-low" : "active-high",
            override->level_triggered ?
                "level" : "edge"
        );
    }
}

static void command_smp(
    const char *arguments
)
{
    (void)arguments;
    smp_print_status();
}

static void command_smpwork(
    const char *arguments
)
{
    uint64_t iterations = 0;

    if (
        arguments != NULL &&
        arguments[0] != '\0' &&
        !parse_uint64(
            arguments,
            &iterations
        )
    )
    {
        kprintf(
            "Usage: smpwork [iterations]\n"
        );
        return;
    }

    uint32_t submitted =
        smp_scheduler_start_benchmark(
            iterations
        );

    if (submitted == 0)
    {
        kprintf(
            "No idle AP workers were available\n"
        );
        return;
    }

    kprintf(
        "Scheduled parallel work on %u APs; use smpjobs to inspect progress\n",
        (unsigned int)submitted
    );
}

static void command_smpjobs(
    const char *arguments
)
{
    (void)arguments;
    smp_scheduler_print_status();
}

static void command_irqinfo(
    const char *arguments
)
{
    (void)arguments;

    kprintf(
        "Interrupt controller: %s\n",
        irq_controller_name()
    );

    kprintf(
        "Local APIC: %s  BSP APIC ID: %u\n",
        lapic_is_enabled() ? "enabled" : "unavailable",
        (unsigned int)lapic_id()
    );

    kprintf(
        "I/O APIC: %s  controllers=%u redirections=%u\n",
        ioapic_is_ready() ? "ready" : "unavailable",
        (unsigned int)ioapic_controller_count(),
        (unsigned int)ioapic_redirection_count()
    );

    const uint8_t important_irqs[] = {
        0, 1, 12
    };

    for (
        uint32_t index = 0;
        index < sizeof(important_irqs);
        index++
    )
    {
        uint32_t gsi;
        bool active_low;
        bool level_triggered;

        if (
            acpi_resolve_isa_irq(
                important_irqs[index],
                &gsi,
                &active_low,
                &level_triggered
            )
        )
        {
            kprintf(
                "IRQ%u -> vector %u GSI%u %s %s\n",
                (unsigned int)important_irqs[index],
                (unsigned int)(32U + important_irqs[index]),
                (unsigned int)gsi,
                active_low ? "low" : "high",
                level_triggered ? "level" : "edge"
            );
        }
    }
}

static void command_storage(
    const char *arguments
)
{
    (void)arguments;
    storage_print_controller();
}

static void command_ahci(
    const char *arguments
)
{
    (void)arguments;
    ahci_print_status();
}

static void command_ahcitest(
    const char *arguments
)
{
    (void)arguments;

    terminal_write_line(
        ahci_run_self_test() ?
            "AHCI DMA read/write self-test: PASSED" :
            "AHCI DMA read/write self-test: FAILED"
    );
}

static void command_nvme(
    const char *arguments
)
{
    (void)arguments;
    nvme_print_status();
}

static void command_nvmetest(
    const char *arguments
)
{
    (void)arguments;

    terminal_write_line(
        nvme_run_self_test() ?
            "NVMe read/write/flush self-test: PASSED" :
            "NVMe read/write/flush self-test: FAILED"
    );
}

static void command_partitions(
    const char *arguments
)
{
    (void)arguments;
    partition_print();
}

static void command_disks(
    const char *arguments
)
{
    (void)arguments;
    storage_print_devices();
}

static void command_diskread(
    const char *arguments
)
{
    char first[24];
    const char *remaining;

    if (
        !split_first_argument(
            arguments,
            first,
            sizeof(first),
            &remaining
        )
    )
    {
        terminal_write_line(
            "Usage: diskread [DISK] LBA"
        );
        return;
    }

    const block_device_t *device = NULL;
    uint64_t lba;

    if (remaining[0] == '\0')
    {
        device = storage_primary_device();

        if (!parse_uint64(first, &lba))
        {
            terminal_write_line(
                "Usage: diskread [DISK] LBA"
            );
            return;
        }
    }
    else
    {
        uint64_t disk_index;

        if (
            !parse_uint64(first, &disk_index) ||
            !parse_uint64(remaining, &lba) ||
            disk_index >= block_device_count()
        )
        {
            terminal_write_line(
                "Usage: diskread [DISK] LBA"
            );
            return;
        }

        device = block_device_get(
            (uint32_t)disk_index
        );
    }

    if (device == NULL)
    {
        terminal_write_line(
            "No block device available"
        );
        return;
    }

    if (
        device->sector_size >
            DISK_SECTOR_BUFFER_SIZE
    )
    {
        terminal_write_line(
            "Sector is larger than the diagnostic buffer"
        );
        return;
    }

    if (
        !block_device_read(
            device,
            lba,
            1,
            disk_read_buffer
        )
    )
    {
        terminal_write_line(
            "Disk read failed"
        );
        return;
    }

    terminal_write("Sector ");

    char lba_text[21];
    uint64_to_string(lba, lba_text);
    terminal_write(lba_text);

    terminal_write(" from ");
    terminal_write(device->name);

    terminal_write_line(
        " first 64 bytes:"
    );

    for (
        uint32_t row = 0;
        row < 4;
        row++
    )
    {
        for (
            uint32_t column = 0;
            column < 16;
            column++
        )
        {
            uint32_t index =
                row * 16 + column;

            terminal_write_hex_byte(
                disk_read_buffer[index]
            );

            terminal_write(" ");
        }

        terminal_write("|");

        for (
            uint32_t column = 0;
            column < 16;
            column++
        )
        {
            uint8_t value =
                disk_read_buffer[
                    row * 16 + column
                ];

            char text[2];

            text[0] =
                value >= 32 && value <= 126 ?
                (char)value :
                '.';

            text[1] = '\0';
            terminal_write(text);
        }

        terminal_write_line("|");
    }
}

static void command_disktest(
    const char *arguments
)
{
    (void)arguments;

    const block_device_t *device =
        storage_primary_device();

    if (device == NULL)
    {
        terminal_write_line(
            "No block device available"
        );
        return;
    }

    if (
        !device->writable ||
        device->sector_size !=
            DISK_SECTOR_BUFFER_SIZE ||
        device->sector_count <=
            DISK_TEST_LBA
    )
    {
        terminal_write_line(
            "Disk cannot run write test"
        );
        return;
    }

    for (
        uint32_t index = 0;
        index < DISK_SECTOR_BUFFER_SIZE;
        index++
    )
    {
        disk_write_buffer[index] =
            (uint8_t)(index ^ 0xA5U);

        disk_read_buffer[index] = 0;
    }

    static const char signature[] =
        "LatterOS block device test";

    for (
        uint32_t index = 0;
        signature[index] != '\0';
        index++
    )
    {
        disk_write_buffer[index] =
            (uint8_t)signature[index];
    }

    if (
        !block_device_write(
            device,
            DISK_TEST_LBA,
            1,
            disk_write_buffer
        )
    )
    {
        terminal_write_line(
            "Disk write failed"
        );
        return;
    }

    if (
        !block_device_read(
            device,
            DISK_TEST_LBA,
            1,
            disk_read_buffer
        )
    )
    {
        terminal_write_line(
            "Disk read-back failed"
        );
        return;
    }

    for (
        uint32_t index = 0;
        index < DISK_SECTOR_BUFFER_SIZE;
        index++
    )
    {
        if (
            disk_write_buffer[index] !=
            disk_read_buffer[index]
        )
        {
            terminal_write_line(
                "Disk verification failed"
            );
            return;
        }
    }

    terminal_write_line(
        "Disk write/read test passed"
    );
}

static void command_ls(
    const char *arguments
)
{
    char parsed_path[VFS_PATH_MAX];
    const char *path = ".";

    if (arguments[0] != '\0')
    {
        if (!parse_single_argument(
                arguments,
                parsed_path,
                sizeof(parsed_path)
            ))
        {
            terminal_write_line(
                "Usage: ls [PATH]"
            );
            return;
        }

        path = parsed_path;
    }

    vfs_node_t *directory = vfs_open(path);

    if (directory == NULL)
    {
        terminal_write_line(
            "Path not found"
        );
        return;
    }

    if (directory->type != VFS_NODE_DIRECTORY)
    {
        terminal_write_line(
            "Not a directory"
        );
        return;
    }

    vfs_node_t *child = directory->first_child;

    if (child == NULL)
    {
        terminal_write_line("(empty)");
        return;
    }

    while (child != NULL)
    {
        terminal_write(
            child->type == VFS_NODE_DIRECTORY ?
                "[DIR]  " :
                "[FILE] "
        );

        terminal_write(child->name);

        if (child->type == VFS_NODE_FILE)
        {
            char size_text[21];
            uint64_to_string(child->size, size_text);
            terminal_write(" (");
            terminal_write(size_text);
            terminal_write(" bytes)");
        }

        terminal_write_line("");
        child = child->next_sibling;
    }
}

static void command_cd(
    const char *arguments
)
{
    char path[VFS_PATH_MAX];

    if (!parse_single_argument(
            arguments,
            path,
            sizeof(path)
        ))
    {
        terminal_write_line(
            "Usage: cd PATH"
        );
        return;
    }

    if (!vfs_change_directory(path))
    {
        terminal_write_line(
            "Directory not found"
        );
    }
}

static void command_pwd(
    const char *arguments
)
{
    (void)arguments;

    char path[VFS_PATH_MAX];

    if (
        !vfs_get_working_directory(
            path,
            sizeof(path)
        )
    )
    {
        terminal_write_line(
            "Unable to resolve path"
        );
        return;
    }

    terminal_write_line(path);
}

static void command_cat(
    const char *arguments
)
{
    char path[VFS_PATH_MAX];

    if (!parse_single_argument(
            arguments,
            path,
            sizeof(path)
        ))
    {
        terminal_write_line(
            "Usage: cat PATH"
        );
        return;
    }

    vfs_node_t *file = vfs_open(path);

    if (file == NULL)
    {
        terminal_write_line(
            "File not found"
        );
        return;
    }

    if (file->type != VFS_NODE_FILE)
    {
        terminal_write_line(
            "Not a file"
        );
        return;
    }

    char buffer[121];
    size_t offset = 0;
    char last_character = '\0';

    while (offset < file->size)
    {
        size_t count = vfs_read(
            file,
            offset,
            buffer,
            sizeof(buffer) - 1
        );

        if (count == 0)
        {
            break;
        }

        buffer[count] = '\0';
        last_character = buffer[count - 1];
        terminal_write(buffer);
        offset += count;
    }

    if (file->size == 0 || last_character != '\n')
    {
        terminal_write_line("");
    }
}

static void command_mkdir(
    const char *arguments
)
{
    char path[VFS_PATH_MAX];

    if (!parse_single_argument(
            arguments,
            path,
            sizeof(path)
        ))
    {
        terminal_write_line(
            "Usage: mkdir PATH"
        );
        return;
    }

    if (!vfs_make_directory(path))
    {
        terminal_write_line(
            "Unable to create directory"
        );
        return;
    }

    terminal_write_line(
        "Directory created"
    );
}

static void command_write(
    const char *arguments
)
{
    char path[VFS_PATH_MAX];
    const char *text;

    if (
        !split_first_argument(
            arguments,
            path,
            sizeof(path),
            &text
        )
    )
    {
        terminal_write_line(
            "Usage: write PATH TEXT"
        );
        return;
    }

    if (!vfs_write_text(path, text))
    {
        terminal_write_line(
            "Unable to write file"
        );
        return;
    }

    terminal_write_line(
        "File written"
    );
}

static void command_rm(
    const char *arguments
)
{
    char path[VFS_PATH_MAX];

    if (!parse_single_argument(
            arguments,
            path,
            sizeof(path)
        ))
    {
        terminal_write_line(
            "Usage: rm PATH"
        );
        return;
    }

    terminal_write_line(
        vfs_remove(path, true) ?
            "Removed" :
            "Unable to remove path"
    );
}

static void command_rename(
    const char *arguments
)
{
    char path[VFS_PATH_MAX];
    char new_name[VFS_NAME_MAX + 1];
    const char *remaining;

    if (
        !split_first_argument(
            arguments,
            path,
            sizeof(path),
            &remaining
        ) ||
        !parse_single_argument(
            remaining,
            new_name,
            sizeof(new_name)
        )
    )
    {
        terminal_write_line(
            "Usage: rename PATH NEW_NAME"
        );
        return;
    }

    terminal_write_line(
        vfs_rename(path, new_name) ?
            "Renamed" :
            "Unable to rename path"
    );
}

static void command_cp(
    const char *arguments
)
{
    char source[VFS_PATH_MAX];
    char destination[VFS_PATH_MAX];
    const char *remaining;

    if (
        !split_first_argument(
            arguments,
            source,
            sizeof(source),
            &remaining
        ) ||
        !parse_single_argument(
            remaining,
            destination,
            sizeof(destination)
        )
    )
    {
        terminal_write_line(
            "Usage: cp SOURCE DESTINATION"
        );
        return;
    }

    terminal_write_line(
        vfs_copy(source, destination) ?
            "Copied" :
            "Unable to copy path"
    );
}

static void command_mv(
    const char *arguments
)
{
    char source[VFS_PATH_MAX];
    char destination[VFS_PATH_MAX];
    const char *remaining;

    if (
        !split_first_argument(
            arguments,
            source,
            sizeof(source),
            &remaining
        ) ||
        !parse_single_argument(
            remaining,
            destination,
            sizeof(destination)
        )
    )
    {
        terminal_write_line(
            "Usage: mv SOURCE DESTINATION"
        );
        return;
    }

    terminal_write_line(
        vfs_move(source, destination) ?
            "Moved" :
            "Unable to move path"
    );
}

static void command_ps(
    const char *arguments
)
{
    (void)arguments;
    process_print_all();
}

static void command_spawn(
    const char *arguments
)
{
    (void)arguments;

    if (
        !security_has_capability(
            SECURITY_CAP_PROCESS_ADMIN
        )
    )
    {
        terminal_write_line("Permission denied");
        return;
    }

    if (process_spawn_demo_thread())
    {
        terminal_write_line(
            "Kernel worker thread created"
        );
    }
    else
    {
        terminal_write_line(
            "Unable to create worker thread"
        );
    }
}

static void command_run(
    const char *arguments
)
{
    if (
        !security_has_capability(
            SECURITY_CAP_PROCESS_SPAWN
        )
    )
    {
        terminal_write_line("Permission denied");
        return;
    }

    if (arguments[0] == '\0')
    {
        terminal_write_line(
            "Usage: run PATH"
        );
        return;
    }

    uint64_t pid =
        process_create_user_program(
            arguments
        );

    if (pid == 0)
    {
        terminal_write_line(
            "Unable to load user program"
        );
        return;
    }

    char pid_text[21];
    uint64_to_string(pid, pid_text);

    terminal_write("Started user process ");
    terminal_write_line(pid_text);
}

static void command_kill(
    const char *arguments
)
{
    if (
        !security_has_capability(
            SECURITY_CAP_PROCESS_ADMIN
        )
    )
    {
        terminal_write_line("Permission denied");
        return;
    }

    uint64_t pid;

    if (!parse_uint64(arguments, &pid))
    {
        terminal_write_line(
            "Usage: kill PID"
        );
        return;
    }

    if (!process_terminate(pid))
    {
        terminal_write_line(
            "Unable to terminate process"
        );
        return;
    }

    terminal_write_line(
        "Process terminated"
    );
}

static void command_libtest(
    const char *arguments
)
{
    (void)arguments;

    char copied[32];
    char number[32];
    char formatted[128];
    int64_t parsed_value;

    kstrcpy(copied, "LatterOS");

    bool parsed =
        kparse_i64(
            "-2048",
            &parsed_value
        );

    kitoa(
        parsed_value,
        number,
        10
    );

    int formatted_length =
        ksnprintf(
            formatted,
            sizeof(formatted),
            "formatted: text=%s signed=%lld hex=%08X",
            copied,
            (long long)parsed_value,
            0x2A
        );

    kprintf(
        "strlen(\"%s\") = %zu\n",
        copied,
        kstrlen(copied)
    );

    kprintf(
        "strcmp equal = %d\n",
        kstrcmp(copied, "LatterOS")
    );

    kprintf(
        "atoi result = %s (%s)\n",
        number,
        parsed ? "valid" : "invalid"
    );

    kprintf(
        "%s\n",
        formatted
    );

    kprintf(
        "snprintf length = %d\n",
        formatted_length
    );
}

static void command_apitest(
    const char *arguments
)
{
    (void)arguments;

    uint64_t pid =
        process_create_user_program(
            "/bin/apitest"
        );

    if (pid == 0)
    {
        terminal_write_line(
            "Unable to start API test"
        );
        return;
    }

    char pid_text[21];
    uint64_to_string(pid, pid_text);

    terminal_write("Started API test process ");
    terminal_write_line(pid_text);
}

static void command_securitytest(
    const char *arguments
)
{
    (void)arguments;

    uint64_t validation_pid =
        process_create_user_program(
            "/bin/sectest"
        );

    uint64_t wx_pid =
        process_create_user_program(
            "/bin/wxtest"
        );

    if (
        validation_pid == 0 ||
        wx_pid == 0
    )
    {
        terminal_write_line(
            "Unable to start security tests"
        );
        return;
    }

    terminal_write_line(
        "Security tests started"
    );

    terminal_write_line(
        "Expected: syscall validation passes and W^X process is terminated without a kernel panic"
    );
}

static void print_capability(
    uint64_t capabilities,
    uint64_t capability,
    const char *name
)
{
    if ((capabilities & capability) != 0)
    {
        terminal_write(" ");
        terminal_write(name);
    }
}

static void command_whoami(
    const char *arguments
)
{
    (void)arguments;
    terminal_write_line(
        security_current_username()
    );
}

static void command_id(
    const char *arguments
)
{
    (void)arguments;

    char line[160];

    ksnprintf(
        line,
        sizeof(line),
        "uid=%u(%s) gid=%u capabilities=0x%llX",
        (unsigned int)security_session_uid(),
        security_current_username(),
        (unsigned int)security_session_gid(),
        (unsigned long long)
            security_session_capabilities()
    );

    terminal_write_line(line);
}

static void command_users(
    const char *arguments
)
{
    (void)arguments;

    for (
        uint32_t index = 0;
        index < security_user_count();
        index++
    )
    {
        security_user_info_t user;

        if (!security_user_get(index, &user))
        {
            continue;
        }

        char line[128];

        ksnprintf(
            line,
            sizeof(line),
            "%s uid=%u gid=%u caps=0x%llX",
            user.name,
            (unsigned int)user.uid,
            (unsigned int)user.gid,
            (unsigned long long)
                user.capabilities
        );

        terminal_write_line(line);
    }
}

static void command_login(
    const char *arguments
)
{
    char name[SECURITY_USER_NAME_MAX + 1];
    const char *password;

    if (
        !split_first_argument(
            arguments,
            name,
            sizeof(name),
            &password
        ) ||
        password[0] == '\0'
    )
    {
        terminal_write_line(
            "Usage: login USER PASSWORD"
        );
        return;
    }

    if (!security_login(name, password))
    {
        terminal_write_line("Login failed");
        return;
    }

    gui_notify_session_changed();
    terminal_write("Logged in as ");
    terminal_write_line(
        security_current_username()
    );
}

static void command_logout(
    const char *arguments
)
{
    (void)arguments;
    security_logout();
    gui_notify_session_changed();
    terminal_write_line(
        "Guest session active"
    );
}

static void command_caps(
    const char *arguments
)
{
    (void)arguments;

    uint64_t capabilities =
        security_session_capabilities();

    terminal_write("Capabilities:");
    print_capability(
        capabilities,
        SECURITY_CAP_FILE_ADMIN,
        "file-admin"
    );
    print_capability(
        capabilities,
        SECURITY_CAP_PROCESS_ADMIN,
        "process-admin"
    );
    print_capability(
        capabilities,
        SECURITY_CAP_SYSTEM_CONTROL,
        "system-control"
    );
    print_capability(
        capabilities,
        SECURITY_CAP_USER_ADMIN,
        "user-admin"
    );
    print_capability(
        capabilities,
        SECURITY_CAP_PROCESS_SPAWN,
        "process-spawn"
    );
    print_capability(
        capabilities,
        SECURITY_CAP_NETWORK_USE,
        "network-use"
    );
    terminal_write_line("");
}

static void command_stat(
    const char *arguments
)
{
    if (arguments[0] == '\0')
    {
        terminal_write_line("Usage: stat PATH");
        return;
    }

    vfs_node_t *node = vfs_open(arguments);

    if (node == NULL)
    {
        terminal_write_line("Path not found");
        return;
    }

    char mode[4];
    mode_to_string(node->mode, mode);

    char line[180];

    ksnprintf(
        line,
        sizeof(line),
        "%s owner=%s uid=%u gid=%u mode=%s size=%llu",
        node->type == VFS_NODE_DIRECTORY ?
            "directory" : "file",
        security_user_name(node->owner_uid),
        (unsigned int)node->owner_uid,
        (unsigned int)node->owner_gid,
        mode,
        (unsigned long long)node->size
    );

    terminal_write_line(line);
}

static void command_chmod(
    const char *arguments
)
{
    char mode_text[8];
    const char *path;
    uint16_t mode;

    if (
        !split_first_argument(
            arguments,
            mode_text,
            sizeof(mode_text),
            &path
        ) ||
        path[0] == '\0' ||
        !parse_octal_mode(mode_text, &mode)
    )
    {
        terminal_write_line(
            "Usage: chmod MODE PATH"
        );
        return;
    }

    terminal_write_line(
        vfs_chmod(path, mode) ?
            "Mode changed" :
            "Permission denied or path invalid"
    );
}

static void command_chown(
    const char *arguments
)
{
    char name[SECURITY_USER_NAME_MAX + 1];
    const char *path;
    security_user_info_t user;

    if (
        !split_first_argument(
            arguments,
            name,
            sizeof(name),
            &path
        ) ||
        path[0] == '\0' ||
        !security_find_user(name, &user)
    )
    {
        terminal_write_line(
            "Usage: chown USER PATH"
        );
        return;
    }

    terminal_write_line(
        vfs_chown(path, user.uid, user.gid) ?
            "Owner changed" :
            "Permission denied or path invalid"
    );
}

static void command_permtest(
    const char *arguments
)
{
    (void)arguments;

    bool passed = true;
    char byte = 0;

    if (!security_login("user", "latteros"))
    {
        passed = false;
    }

    (void)vfs_remove("/home/.permtest", true);

    if (
        !vfs_write_text(
            "/home/.permtest",
            "private"
        ) ||
        !vfs_chmod(
            "/home/.permtest",
            0600U
        )
    )
    {
        passed = false;
    }

    vfs_node_t *node =
        vfs_open("/home/.permtest");

    if (
        node == NULL ||
        vfs_read(node, 0, &byte, 1) != 1
    )
    {
        passed = false;
    }

    if (!security_login("guest", "guest"))
    {
        passed = false;
    }

    if (
        node != NULL &&
        vfs_read(node, 0, &byte, 1) != 0
    )
    {
        passed = false;
    }

    if (
        security_has_capability(
            SECURITY_CAP_SYSTEM_CONTROL
        ) ||
        security_has_capability(
            SECURITY_CAP_FILE_ADMIN
        )
    )
    {
        passed = false;
    }

    if (!security_login("root", "root"))
    {
        passed = false;
    }

    if (
        node == NULL ||
        vfs_read(node, 0, &byte, 1) != 1 ||
        !vfs_remove("/home/.permtest", true)
    )
    {
        passed = false;
    }

    (void)security_login("user", "latteros");
    gui_notify_session_changed();

    terminal_write_line(
        passed ?
            "Permission and capability tests: PASSED" :
            "Permission and capability tests: FAILED"
    );
}

static void command_mouse(
    const char *arguments
)
{
    (void)arguments;

    if (!mouse_is_available())
    {
        terminal_write_line(
            "PS/2 mouse is unavailable"
        );
        return;
    }

    mouse_state_t state;
    mouse_get_state(&state);

    kprintf(
        "Mouse x=%d y=%d packets=%llu left=%s right=%s middle=%s\n",
        state.x,
        state.y,
        (unsigned long long)state.packet_count,
        state.left_button ? "down" : "up",
        state.right_button ? "down" : "up",
        state.middle_button ? "down" : "up"
    );
}

static void command_beep(
    const char *arguments
)
{
    uint64_t frequency = 440;
    uint64_t duration = 250;

    if (arguments[0] != '\0')
    {
        char frequency_text[21];
        const char *remaining;

        if (
            !split_first_argument(
                arguments,
                frequency_text,
                sizeof(frequency_text),
                &remaining
            ) ||
            !parse_uint64(
                frequency_text,
                &frequency
            )
        )
        {
            terminal_write_line(
                "Usage: beep [frequency] [milliseconds]"
            );
            return;
        }

        if (
            remaining[0] != '\0' &&
            !parse_uint64(
                remaining,
                &duration
            )
        )
        {
            terminal_write_line(
                "Usage: beep [frequency] [milliseconds]"
            );
            return;
        }
    }

    if (
        frequency < 20 ||
        frequency > 20000 ||
        duration < 10 ||
        duration > 5000
    )
    {
        terminal_write_line(
            "Frequency 20-20000 Hz, duration 10-5000 ms"
        );
        return;
    }

    kprintf(
        "Beeping at %llu Hz for %llu ms\n",
        (unsigned long long)frequency,
        (unsigned long long)duration
    );

    speaker_beep(
        (uint32_t)frequency,
        (uint32_t)duration
    );
}

static void command_date(
    const char *arguments
)
{
    (void)arguments;

    rtc_datetime_t datetime;

    if (!rtc_read(&datetime))
    {
        terminal_write_line(
            "Unable to read RTC"
        );
        return;
    }

    char text[32];

    ksnprintf(
        text,
        sizeof(text),
        "%04u-%02u-%02u %02u:%02u:%02u UTC",
        (uint32_t)datetime.year,
        (uint32_t)datetime.month,
        (uint32_t)datetime.day,
        (uint32_t)datetime.hour,
        (uint32_t)datetime.minute,
        (uint32_t)datetime.second
    );

    terminal_write_line(text);
}

static void command_serialtest(
    const char *arguments
)
{
    (void)arguments;

    if (!serial_self_test())
    {
        terminal_write_line(
            "Serial COM1 loopback: failed"
        );
        return;
    }

    serial_write_line(
        "LatterOS COM1 serial driver is working."
    );

    terminal_write_line(
        "Serial COM1 loopback: passed"
    );
}

static void command_usb(
    const char *arguments
)
{
    (void)arguments;
    usb_print_status();
}

static void command_usbms(
    const char *arguments
)
{
    (void)arguments;
    usb_mass_storage_print();
}

static void command_fatinfo(
    const char *arguments
)
{
    (void)arguments;
    fat_fs_print_status();
}

static void command_mountusb(
    const char *arguments
)
{
    (void)arguments;

    if (fat_fs_mounted())
    {
        (void)fat_fs_unmount();
    }

    terminal_write_line(
        fat_fs_mount_first_usb() ?
            "USB FAT volume mounted at /media/usb" :
            "Unable to mount USB FAT volume"
    );
}

static void command_mountsata(
    const char *arguments
)
{
    (void)arguments;

    if (fat_fs_mounted())
    {
        (void)fat_fs_unmount();
    }

    terminal_write_line(
        fat_fs_mount_first_sata() ?
            "AHCI FAT volume mounted at /media/sata" :
            "Unable to mount AHCI FAT volume"
    );
}

static void command_mountnvme(
    const char *arguments
)
{
    (void)arguments;

    if (fat_fs_mounted())
    {
        (void)fat_fs_unmount();
    }

    terminal_write_line(
        fat_fs_mount_first_nvme() ?
            "NVMe FAT volume mounted at /media/nvme" :
            "Unable to mount NVMe FAT volume"
    );
}

static void command_umountusb(
    const char *arguments
)
{
    (void)arguments;

    terminal_write_line(
        fat_fs_unmount() ?
            "FAT volume synchronized and unmounted" :
            "Unable to unmount FAT volume"
    );
}

static void command_fatrwtest(
    const char *arguments
)
{
    (void)arguments;

    terminal_write_line(
        fat_fs_run_write_test() ?
            "FAT complete read/write test: PASSED" :
            "FAT complete read/write test: FAILED"
    );
}

static void command_net(
    const char *arguments
)
{
    (void)arguments;
    network_print_status();
}

static void command_netsend(
    const char *arguments
)
{
    (void)arguments;

    if (network_send_test_frame())
    {
        terminal_write_line(
            "Raw Ethernet frame transmitted"
        );
    }
    else
    {
        terminal_write_line(
            "Ethernet transmission failed"
        );
    }
}

static void command_ifconfig(
    const char *arguments
)
{
    (void)arguments;

    network_print_status();
    ipv4_print_config();
}

static void command_arp(
    const char *arguments
)
{
    if (arguments[0] == '\0')
    {
        arp_print_cache();
        return;
    }

    uint32_t address;

    if (
        !ipv4_parse_address(
            arguments,
            &address
        )
    )
    {
        terminal_write_line(
            "Usage: arp [IPv4]"
        );

        return;
    }

    uint8_t mac[6];

    if (
        !arp_resolve(
            address,
            mac,
            1000
        )
    )
    {
        terminal_write_line(
            "ARP resolution timed out"
        );

        return;
    }

    char address_text[16];

    ipv4_format_address(
        address,
        address_text,
        sizeof(address_text)
    );

    kprintf(
        "%s -> %02X:%02X:%02X:%02X:%02X:%02X\n",
        address_text,
        (uint32_t)mac[0],
        (uint32_t)mac[1],
        (uint32_t)mac[2],
        (uint32_t)mac[3],
        (uint32_t)mac[4],
        (uint32_t)mac[5]
    );
}

static void command_ping(
    const char *arguments
)
{
    uint32_t address;

    if (
        !ipv4_parse_address(
            arguments,
            &address
        )
    )
    {
        terminal_write_line(
            "Usage: ping IPv4"
        );

        return;
    }

    char address_text[16];

    ipv4_format_address(
        address,
        address_text,
        sizeof(address_text)
    );

    kprintf(
        "PING %s\n",
        address_text
    );

    uint32_t elapsed = 0;

    if (
        icmp_ping(
            address,
            1500,
            &elapsed
        )
    )
    {
        kprintf(
            "Reply from %s: time=%u ms\n",
            address_text,
            elapsed
        );
    }
    else
    {
        kprintf(
            "Request to %s timed out\n",
            address_text
        );
    }
}

static void command_udp(
    const char *arguments
)
{
    (void)arguments;
    udp_print_status();
}

static void command_dhcp(
    const char *arguments
)
{
    (void)arguments;

    terminal_write_line(
        "Requesting DHCP lease..."
    );

    if (!dhcp_configure(5000000U))
    {
        terminal_write_line(
            "DHCP request failed"
        );
        return;
    }

    dns_set_server(
        dhcp_dns_server()
    );

    terminal_write_line(
        "DHCP lease acquired"
    );

    dhcp_print_status();
    ipv4_print_config();
}

static void command_dns(
    const char *arguments
)
{
    if (arguments[0] == '\0')
    {
        dns_print_server();
        return;
    }

    uint32_t address;

    if (
        !dns_resolve_a(
            arguments,
            5000000U,
            &address
        )
    )
    {
        kprintf(
            "DNS lookup failed: %s\n",
            arguments
        );
        return;
    }

    char address_text[16];

    ipv4_format_address(
        address,
        address_text,
        sizeof(address_text)
    );

    kprintf(
        "%s -> %s\n",
        arguments,
        address_text
    );
}


static bool resolve_network_name(
    const char *name,
    uint32_t *address
)
{
    if (
        name == NULL ||
        address == NULL ||
        name[0] == '\0'
    )
    {
        return false;
    }

    if (
        ipv4_parse_address(
            name,
            address
        )
    )
    {
        return true;
    }

    return dns_resolve_a(
        name,
        5000000U,
        address
    );
}

static void command_tcp(
    const char *arguments
)
{
    char host[128];
    const char *port_text;

    if (
        !split_first_argument(
            arguments,
            host,
            sizeof(host),
            &port_text
        )
    )
    {
        if (arguments[0] == '\0')
        {
            tcp_print_status();
        }
        else
        {
            terminal_write_line(
                "Usage: tcp HOST [PORT]"
            );
        }

        return;
    }

    uint64_t port = 80;

    if (
        port_text[0] != '\0' &&
        (
            !parse_uint64(
                port_text,
                &port
            ) ||
            port == 0 ||
            port > 65535
        )
    )
    {
        terminal_write_line(
            "Usage: tcp HOST [PORT]"
        );
        return;
    }

    uint32_t address;

    if (!resolve_network_name(host, &address))
    {
        kprintf(
            "Unable to resolve %s\n",
            host
        );
        return;
    }

    char address_text[16];

    ipv4_format_address(
        address,
        address_text,
        sizeof(address_text)
    );

    kprintf(
        "Connecting to %s:%u...\n",
        address_text,
        (uint32_t)port
    );

    if (
        !tcp_connect(
            address,
            (uint16_t)port,
            5000
        )
    )
    {
        terminal_write_line(
            "TCP connection failed"
        );
        return;
    }

    terminal_write_line(
        "TCP connection established"
    );

    tcp_close(1500);

    terminal_write_line(
        "TCP connection closed"
    );
}

static void command_http(
    const char *arguments
)
{
    char host[128];
    const char *path;

    if (
        !split_first_argument(
            arguments,
            host,
            sizeof(host),
            &path
        )
    )
    {
        terminal_write_line(
            "Usage: http HOST [PATH]"
        );
        return;
    }

    if (path[0] == '\0')
    {
        path = "/";
    }

    if (path[0] != '/')
    {
        terminal_write_line(
            "HTTP path must start with /"
        );
        return;
    }

    uint32_t address;

    if (!resolve_network_name(host, &address))
    {
        kprintf(
            "Unable to resolve %s\n",
            host
        );
        return;
    }

    kprintf(
        "Connecting to %s...\n",
        host
    );

    if (!tcp_connect(address, 80, 5000))
    {
        terminal_write_line(
            "HTTP TCP connection failed"
        );
        return;
    }

    char request[512];

    int request_length =
        ksnprintf(
            request,
            sizeof(request),
            "GET %s HTTP/1.0\r\n"
            "Host: %s\r\n"
            "User-Agent: LatterOS/0.1\r\n"
            "Connection: close\r\n"
            "\r\n",
            path,
            host
        );

    if (
        request_length <= 0 ||
        (size_t)request_length >=
            sizeof(request) ||
        !tcp_send_data(
            request,
            (size_t)request_length,
            5000
        )
    )
    {
        terminal_write_line(
            "Unable to send HTTP request"
        );
        tcp_abort();
        return;
    }

    terminal_write_line(
        "--- HTTP response ---"
    );

    uint8_t received[256];
    char printable[257];
    size_t total = 0;
    bool received_anything = false;

    while (total < 4096)
    {
        size_t length =
            tcp_receive_data(
                received,
                sizeof(received),
                received_anything ?
                    1000U : 5000U
            );

        if (length == 0)
        {
            if (
                tcp_peer_closed() ||
                received_anything
            )
            {
                break;
            }

            terminal_write_line(
                "HTTP response timed out"
            );
            break;
        }

        received_anything = true;

        size_t output = 0;

        for (
            size_t index = 0;
            index < length &&
            total < 4096;
            index++, total++
        )
        {
            uint8_t character =
                received[index];

            if (character == '\r')
            {
                continue;
            }

            if (
                character == '\n' ||
                character == '\t' ||
                (
                    character >= 32 &&
                    character <= 126
                )
            )
            {
                printable[output++] =
                    character == '\t' ?
                    ' ' : (char)character;
            }
            else
            {
                printable[output++] = '.';
            }
        }

        printable[output] = '\0';
        terminal_write(printable);

        if (tcp_peer_closed())
        {
            break;
        }
    }

    terminal_write_line("");
    terminal_write_line(
        "--- end response ---"
    );

    tcp_close(1500);
}

static void command_fsinfo(
    const char *arguments
)
{
    (void)arguments;
    latteros_fs_print_info();
}

static void command_sync(
    const char *arguments
)
{
    (void)arguments;

    bool system_synced = latteros_fs_sync();
    bool fat_synced =
        !fat_fs_mounted() || fat_fs_sync();

    terminal_write_line(
        system_synced && fat_synced ?
            "All mounted filesystems synchronized" :
            "One or more filesystems could not be synchronized"
    );
}

static bool parse_log_level(
    const char *text,
    klog_level_t *level
)
{
    if (
        text == NULL ||
        level == NULL
    )
    {
        return false;
    }

    if (strings_equal(text, "debug"))
    {
        *level = KLOG_DEBUG;
        return true;
    }

    if (strings_equal(text, "info"))
    {
        *level = KLOG_INFO;
        return true;
    }

    if (
        strings_equal(text, "warn") ||
        strings_equal(text, "warning")
    )
    {
        *level = KLOG_WARNING;
        return true;
    }

    if (strings_equal(text, "error"))
    {
        *level = KLOG_ERROR;
        return true;
    }

    if (strings_equal(text, "panic"))
    {
        *level = KLOG_PANIC;
        return true;
    }

    return false;
}

static void command_dmesg(
    const char *arguments
)
{
    while (
        arguments != NULL &&
        character_is_space(*arguments)
    )
    {
        arguments++;
    }

    if (
        arguments != NULL &&
        strings_equal(arguments, "clear")
    )
    {
        klog_clear();
        terminal_write_line("Kernel log cleared");
        return;
    }

    klog_level_t level = KLOG_DEBUG;

    if (
        arguments != NULL &&
        arguments[0] != '\0' &&
        !parse_log_level(arguments, &level)
    )
    {
        terminal_write_line(
            "Usage: dmesg [debug|info|warn|error|panic|clear]"
        );

        return;
    }

    klog_print(level);
}

static void command_logtest(
    const char *arguments
)
{
    (void)arguments;

    klog_write(KLOG_DEBUG, "test", "Debug log message");
    klog_write(KLOG_INFO, "test", "Information log message");
    klog_write(KLOG_WARNING, "test", "Warning log message");
    klog_write(KLOG_ERROR, "test", "Error log message");

    terminal_write_line(
        "Test messages written; run dmesg"
    );
}

static void command_sysinfo(
    const char *arguments
)
{
    (void)arguments;

    uint32_t frequency = timer_frequency();
    uint64_t uptime_seconds =
        frequency == 0 ?
        0 :
        timer_ticks() / frequency;

    kprintf("LatterOS version 0.1\n");
    kprintf(
        "Uptime: %llu seconds\n",
        (unsigned long long)uptime_seconds
    );
    kprintf(
        "Memory: total=%llu MiB usable=%llu MiB free-pages=%llu\n",
        (unsigned long long)(
            physical_memory_total_bytes() /
            (1024ULL * 1024ULL)
        ),
        (unsigned long long)(
            physical_memory_usable_bytes() /
            (1024ULL * 1024ULL)
        ),
        (unsigned long long)free_page_count()
    );
    kprintf(
        "Processes: %u\n",
        (unsigned int)process_count()
    );

    heap_stats_t heap_stats;
    heap_get_stats(&heap_stats);

    kprintf(
        "Heap: active=%llu bytes=%llu peak=%llu pages=%llu\n",
        (unsigned long long)heap_stats.active_allocations,
        (unsigned long long)heap_stats.active_bytes,
        (unsigned long long)heap_stats.peak_active_bytes,
        (unsigned long long)heap_stats.heap_pages
    );
    kprintf(
        "Filesystem: %s entries=%u used=%llu/%llu bytes\n",
        latteros_fs_is_mounted() ?
            "mounted" : "fallback",
        (unsigned int)latteros_fs_entry_count(),
        (unsigned long long)latteros_fs_used_bytes(),
        (unsigned long long)latteros_fs_capacity_bytes()
    );
    kprintf(
        "Network: %s link=%s rx=%llu tx=%llu\n",
        network_is_ready() ? "ready" : "offline",
        network_link_up() ? "up" : "down",
        (unsigned long long)network_received_frames(),
        (unsigned long long)network_transmitted_frames()
    );
    kprintf(
        "Serial: %s  Kernel log entries: %u\n",
        serial_is_available() ? "ready" : "unavailable",
        (unsigned int)klog_entry_count()
    );
}

static void command_trace(
    const char *arguments
)
{
    (void)arguments;
    stacktrace_print_current();
}

static void command_meminfo(
    const char *arguments
)
{
    heap_print_stats();

    if (strings_equal(arguments, "leaks"))
    {
        heap_print_allocations(32);
    }
}

static void command_memtest(
    const char *arguments
)
{
    (void)arguments;
    (void)selftest_memory();
}

static void command_guardtest(
    const char *arguments
)
{
    (void)arguments;
    (void)selftest_guard_pages();
}

static void command_schedtest(
    const char *arguments
)
{
    uint64_t workers = 6;

    if (
        arguments[0] != '\0' &&
        (
            !parse_uint64(arguments, &workers) ||
            workers == 0 ||
            workers > 8
        )
    )
    {
        terminal_write_line(
            "Usage: schedtest [1-8]"
        );
        return;
    }

    if (
        !selftest_scheduler_start(
            (uint32_t)workers,
            750000ULL
        )
    )
    {
        terminal_write_line(
            "Unable to start scheduler stress test"
        );
        return;
    }

    kprintf(
        "Scheduler stress started with %llu workers. Run schedstatus.\n",
        (unsigned long long)workers
    );
}

static void command_schedstatus(
    const char *arguments
)
{
    (void)arguments;
    selftest_scheduler_print_status();
}

static void command_fstest(
    const char *arguments
)
{
    (void)arguments;
    (void)selftest_filesystem();
}

static void command_nettest(
    const char *arguments
)
{
    (void)arguments;
    (void)selftest_network();
}

static void command_reboot(
    const char *arguments
)
{
    (void)arguments;

    if (
        !security_has_capability(
            SECURITY_CAP_SYSTEM_CONTROL
        )
    )
    {
        terminal_write_line("Permission denied");
        return;
    }

    terminal_write_line("Restarting LatterOS...");
    power_reboot();
}

static void command_gui(
    const char *arguments
)
{
    (void)arguments;

    gui_request_start();
}

static void command_shutdown(
    const char *arguments
)
{
    (void)arguments;

    if (
        !security_has_capability(
            SECURITY_CAP_SYSTEM_CONTROL
        )
    )
    {
        terminal_write_line("Permission denied");
        return;
    }

    terminal_write_line("Shutting down...");
    power_shutdown();
}

static void command_panic(
    const char *arguments
)
{
    (void)arguments;
    __asm__ volatile("int3");
}

void shell_execute(
    const char *input
)
{
    char command_name[32];

    uint32_t input_index = 0;
    uint32_t command_index = 0;

    while (
        character_is_space(
            input[input_index]
        )
    )
    {
        input_index++;
    }

    while (
        input[input_index] != '\0' &&
        !character_is_space(
            input[input_index]
        ) &&
        command_index <
            sizeof(command_name) - 1
    )
    {
        command_name[command_index] =
            input[input_index];

        command_index++;
        input_index++;
    }

    command_name[command_index] = '\0';

    if (command_index == 0)
    {
        return;
    }

    while (
        character_is_space(
            input[input_index]
        )
    )
    {
        input_index++;
    }

    const char *arguments =
        &input[input_index];

    for (
        uint32_t index = 0;
        index < COMMAND_COUNT;
        index++
    )
    {
        if (
            strings_equal(
                command_name,
                commands[index].name
            )
        )
        {
            commands[index].function(
                arguments
            );

            return;
        }
    }

    terminal_write(
        "Unknown command: "
    );

    terminal_write_line(
        command_name
    );
}
