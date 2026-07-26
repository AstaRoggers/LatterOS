#include "selftest.h"

#include "arp.h"
#include "dns.h"
#include "heap.h"
#include "icmp.h"
#include "ipv4.h"
#include "klog.h"
#include "kstdio.h"
#include "kstring.h"
#include "latteros_fs.h"
#include "network.h"
#include "process.h"
#include "tcp.h"
#include "udp.h"
#include "vfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SCHEDULER_TEST_MAX_WORKERS 8
#define FILESYSTEM_TEST_ROOT "/home/.latteros-selftest"

typedef struct
{
    volatile uint64_t progress;
    uint64_t iterations;
    uint32_t index;
    volatile bool complete;
} scheduler_worker_t;

static scheduler_worker_t scheduler_workers[
    SCHEDULER_TEST_MAX_WORKERS
];

static volatile uint32_t scheduler_worker_count;
static volatile uint32_t scheduler_completed;
static volatile bool scheduler_running;

static void scheduler_test_worker(void *argument)
{
    scheduler_worker_t *worker =
        (scheduler_worker_t *)argument;

    for (
        uint64_t iteration = 0;
        iteration < worker->iterations;
        iteration++
    )
    {
        worker->progress++;

        if ((iteration & 0xFFULL) == 0)
        {
            __asm__ volatile(
                "pause"
                :
                :
                : "memory"
            );
        }

        if ((iteration & 0x3FFULL) == 0)
        {
            __asm__ volatile(
                "int $0x81"
                :
                :
                : "memory"
            );
        }
    }

    worker->complete = true;
    scheduler_completed++;

    if (
        scheduler_completed >=
        scheduler_worker_count
    )
    {
        scheduler_running = false;

        klogf(
            KLOG_INFO,
            "selftest",
            "scheduler stress completed workers=%u",
            (unsigned int)scheduler_worker_count
        );
    }

    process_exit_current();
}

bool selftest_scheduler_start(
    uint32_t worker_count,
    uint64_t iterations
)
{
    if (
        scheduler_running ||
        worker_count == 0 ||
        worker_count > SCHEDULER_TEST_MAX_WORKERS ||
        iterations == 0
    )
    {
        return false;
    }

    scheduler_worker_count = worker_count;
    scheduler_completed = 0;
    scheduler_running = true;

    for (
        uint32_t index = 0;
        index < worker_count;
        index++
    )
    {
        scheduler_workers[index].progress = 0;
        scheduler_workers[index].iterations =
            iterations;
        scheduler_workers[index].index = index;
        scheduler_workers[index].complete = false;

        char name[PROCESS_NAME_LENGTH] =
            "stress-0";

        name[7] =
            (char)('1' + index);

        if (
            !process_create_kernel_thread(
                name,
                scheduler_test_worker,
                &scheduler_workers[index]
            )
        )
        {
            scheduler_worker_count = index;
            scheduler_running =
                index != 0;

            return false;
        }
    }

    klogf(
        KLOG_INFO,
        "selftest",
        "scheduler stress started workers=%u iterations=%llu",
        (unsigned int)worker_count,
        (unsigned long long)iterations
    );

    return true;
}

bool selftest_scheduler_running(void)
{
    return scheduler_running;
}

bool selftest_scheduler_passed(void)
{
    if (
        scheduler_running ||
        scheduler_worker_count == 0 ||
        scheduler_completed !=
            scheduler_worker_count
    )
    {
        return false;
    }

    for (
        uint32_t index = 0;
        index < scheduler_worker_count;
        index++
    )
    {
        const scheduler_worker_t *worker =
            &scheduler_workers[index];

        if (
            !worker->complete ||
            worker->progress !=
                worker->iterations
        )
        {
            return false;
        }
    }

    return true;
}

void selftest_scheduler_print_status(void)
{
    if (scheduler_worker_count == 0)
    {
        kprintf(
            "Scheduler stress test has not been started.\n"
        );
        return;
    }

    kprintf(
        "Scheduler stress: %u/%u complete (%s)\n",
        (unsigned int)scheduler_completed,
        (unsigned int)scheduler_worker_count,
        scheduler_running ? "running" : "finished"
    );

    bool passed = !scheduler_running;

    for (
        uint32_t index = 0;
        index < scheduler_worker_count;
        index++
    )
    {
        const scheduler_worker_t *worker =
            &scheduler_workers[index];

        kprintf(
            "  worker %u: %llu/%llu %s\n",
            (unsigned int)(index + 1),
            (unsigned long long)worker->progress,
            (unsigned long long)worker->iterations,
            worker->complete ? "done" : "running"
        );

        if (
            !worker->complete ||
            worker->progress !=
                worker->iterations
        )
        {
            passed = false;
        }
    }

    if (!scheduler_running)
    {
        kprintf(
            "Scheduler stress result: %s\n",
            passed ? "PASSED" : "FAILED"
        );
    }
}

bool selftest_memory(void)
{
    heap_stats_t before;
    heap_stats_t after;

    heap_get_stats(&before);

    void *first = kmalloc(37);
    void *second = kmalloc(511);
    void *third = kmalloc(1024);

    bool passed =
        first != NULL &&
        second != NULL &&
        third != NULL;

    if (first != NULL)
    {
        uint8_t *bytes = first;

        for (size_t index = 0; index < 37; index++)
        {
            bytes[index] = (uint8_t)(index ^ 0xA5U);
        }

        for (size_t index = 0; index < 37; index++)
        {
            if (
                bytes[index] !=
                (uint8_t)(index ^ 0xA5U)
            )
            {
                passed = false;
            }
        }
    }

    kfree(third);
    kfree(second);
    kfree(first);

    heap_get_stats(&after);

    if (
        before.active_allocations !=
            after.active_allocations ||
        before.active_bytes !=
            after.active_bytes ||
        !heap_validate()
    )
    {
        passed = false;
    }

    kprintf(
        "Memory tracking test: %s\n",
        passed ? "PASSED" : "FAILED"
    );

    heap_print_stats();

    klogf(
        passed ? KLOG_INFO : KLOG_ERROR,
        "selftest",
        "memory tracking %s",
        passed ? "passed" : "failed"
    );

    return passed;
}

bool selftest_guard_pages(void)
{
    bool passed =
        process_guard_pages_validate();

    kprintf(
        "Guard pages: %s (%u guarded stacks)\n",
        passed ? "PASSED" : "FAILED",
        (unsigned int)
            process_guarded_stack_count()
    );

    kprintf(
        "Guard pages remain deliberately unmapped; an overflow will page fault.\n"
    );

    klogf(
        passed ? KLOG_INFO : KLOG_ERROR,
        "selftest",
        "guard pages %s guarded=%u",
        passed ? "passed" : "failed",
        (unsigned int)
            process_guarded_stack_count()
    );

    return passed;
}

static bool read_text_equals(
    const char *path,
    const char *expected
)
{
    vfs_node_t *node = vfs_open(path);

    if (
        node == NULL ||
        node->type != VFS_NODE_FILE ||
        node->size >= 128
    )
    {
        return false;
    }

    char buffer[128];

    size_t count = vfs_read(
        node,
        0,
        buffer,
        sizeof(buffer) - 1
    );

    buffer[count] = '\0';

    return kstrcmp(buffer, expected) == 0;
}

bool selftest_filesystem(void)
{
    (void)vfs_remove(
        FILESYSTEM_TEST_ROOT,
        true
    );

    bool passed =
        vfs_make_directory(
            FILESYSTEM_TEST_ROOT
        ) &&
        vfs_write_text(
            FILESYSTEM_TEST_ROOT "/source.txt",
            "LatterOS filesystem self-test"
        ) &&
        read_text_equals(
            FILESYSTEM_TEST_ROOT "/source.txt",
            "LatterOS filesystem self-test"
        ) &&
        vfs_copy(
            FILESYSTEM_TEST_ROOT "/source.txt",
            FILESYSTEM_TEST_ROOT "/copy.txt"
        ) &&
        vfs_rename(
            FILESYSTEM_TEST_ROOT "/copy.txt",
            "renamed.txt"
        ) &&
        vfs_make_directory(
            FILESYSTEM_TEST_ROOT "/archive"
        ) &&
        vfs_move(
            FILESYSTEM_TEST_ROOT "/renamed.txt",
            FILESYSTEM_TEST_ROOT "/archive"
        ) &&
        read_text_equals(
            FILESYSTEM_TEST_ROOT
                "/archive/renamed.txt",
            "LatterOS filesystem self-test"
        ) &&
        vfs_copy(
            FILESYSTEM_TEST_ROOT "/archive",
            FILESYSTEM_TEST_ROOT "/archive-copy"
        ) &&
        read_text_equals(
            FILESYSTEM_TEST_ROOT
                "/archive-copy/renamed.txt",
            "LatterOS filesystem self-test"
        ) &&
        latteros_fs_sync();

    bool removed =
        vfs_remove(
            FILESYSTEM_TEST_ROOT,
            true
        );

    passed =
        passed &&
        removed &&
        vfs_open(FILESYSTEM_TEST_ROOT) == NULL &&
        latteros_fs_sync();

    kprintf(
        "Filesystem test: %s\n",
        passed ? "PASSED" : "FAILED"
    );

    klogf(
        passed ? KLOG_INFO : KLOG_ERROR,
        "selftest",
        "filesystem %s",
        passed ? "passed" : "failed"
    );

    return passed;
}

bool selftest_network(void)
{
    if (
        !network_is_ready() ||
        !network_link_up()
    )
    {
        kprintf(
            "Network test: FAILED (driver or link unavailable)\n"
        );
        return false;
    }

    bool passed = true;
    uint8_t gateway_mac[6];
    uint32_t gateway = ipv4_gateway();

    kprintf("Network test: ARP gateway... ");

    bool arp_ok = arp_resolve(
        gateway,
        gateway_mac,
        2000
    );

    kprintf("%s\n", arp_ok ? "passed" : "FAILED");
    passed = passed && arp_ok;

    uint32_t elapsed = 0;
    kprintf("Network test: ICMP gateway... ");

    bool ping_ok = icmp_ping(
        gateway,
        2000,
        &elapsed
    );

    if (ping_ok)
    {
        kprintf("passed (%u ms)\n", elapsed);
    }
    else
    {
        kprintf("FAILED\n");
    }

    passed = passed && ping_ok;

    uint64_t udp_before =
        udp_transmitted_datagrams();

    uint32_t example_address = 0;
    kprintf("Network test: DNS example.com... ");

    bool dns_ok = dns_resolve_a(
        "example.com",
        5000000U,
        &example_address
    );

    if (dns_ok)
    {
        char text[16];
        ipv4_format_address(
            example_address,
            text,
            sizeof(text)
        );
        kprintf("passed (%s)\n", text);
    }
    else
    {
        kprintf("FAILED\n");
    }

    bool udp_ok =
        udp_transmitted_datagrams() >
        udp_before;

    passed = passed && dns_ok && udp_ok;

    kprintf("Network test: TCP port 80... ");

    bool tcp_ok =
        dns_ok &&
        tcp_connect(
            example_address,
            80,
            4000
        );

    if (tcp_ok)
    {
        tcp_close(2000);
        kprintf("passed\n");
    }
    else
    {
        tcp_abort();
        kprintf("FAILED\n");
    }

    passed = passed && tcp_ok;

    kprintf(
        "Network test result: %s\n",
        passed ? "PASSED" : "FAILED"
    );

    klogf(
        passed ? KLOG_INFO : KLOG_ERROR,
        "selftest",
        "network %s arp=%u ping=%u dns=%u udp=%u tcp=%u",
        passed ? "passed" : "failed",
        arp_ok ? 1U : 0U,
        ping_ok ? 1U : 0U,
        dns_ok ? 1U : 0U,
        udp_ok ? 1U : 0U,
        tcp_ok ? 1U : 0U
    );

    return passed;
}
