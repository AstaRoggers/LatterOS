#include "autotest.h"

#include "klog.h"
#include "power.h"
#include "selftest.h"
#include "timer.h"

#include <stdbool.h>
#include <stdint.h>

#define AUTOTEST_BOOT_DELAY_TICKS 50ULL
#define AUTOTEST_SCHEDULER_WORKERS 8U
#define AUTOTEST_SCHEDULER_ITERATIONS 750000ULL
#define AUTOTEST_SCHEDULER_TIMEOUT_SECONDS 20ULL

typedef enum
{
    AUTOTEST_IDLE,
    AUTOTEST_WAITING_FOR_BOOT,
    AUTOTEST_WAITING_FOR_SCHEDULER,
    AUTOTEST_FINISHED
} autotest_state_t;

static autotest_state_t state;
static uint64_t state_started_at;
static bool all_passed;

static void report_result(
    const char *name,
    bool passed
)
{
    klogf(
        passed ? KLOG_INFO : KLOG_ERROR,
        "autotest",
        "AUTOTEST: %s %s",
        name,
        passed ? "PASS" : "FAIL"
    );

    if (!passed)
    {
        all_passed = false;
    }
}

static void finish_tests(void)
{
    state = AUTOTEST_FINISHED;

    klog_write(
        all_passed ? KLOG_INFO : KLOG_ERROR,
        "autotest",
        all_passed ?
            "AUTOTEST: PASS" :
            "AUTOTEST: FAIL"
    );

    power_shutdown();
}

void autotest_start(void)
{
    all_passed = true;
    state = AUTOTEST_WAITING_FOR_BOOT;
    state_started_at = timer_ticks();

    klog_write(
        KLOG_INFO,
        "autotest",
        "AUTOTEST: BEGIN"
    );
}

void autotest_update(void)
{
    uint64_t now = timer_ticks();

    if (state == AUTOTEST_WAITING_FOR_BOOT)
    {
        if (
            now - state_started_at <
            AUTOTEST_BOOT_DELAY_TICKS
        )
        {
            return;
        }

        report_result(
            "MEMORY",
            selftest_memory()
        );

        report_result(
            "FILESYSTEM",
            selftest_filesystem()
        );

        bool scheduler_started =
            selftest_scheduler_start(
                AUTOTEST_SCHEDULER_WORKERS,
                AUTOTEST_SCHEDULER_ITERATIONS
            );

        report_result(
            "SCHEDULER_START",
            scheduler_started
        );

        if (!scheduler_started)
        {
            report_result(
                "SCHEDULER",
                false
            );

            finish_tests();
            return;
        }

        report_result(
            "GUARD_PAGES",
            selftest_guard_pages()
        );

        state =
            AUTOTEST_WAITING_FOR_SCHEDULER;

        state_started_at = now;
        return;
    }

    if (state == AUTOTEST_WAITING_FOR_SCHEDULER)
    {
        if (!selftest_scheduler_running())
        {
            report_result(
                "SCHEDULER",
                selftest_scheduler_passed()
            );

            report_result(
                "NETWORK",
                selftest_network()
            );

            finish_tests();
            return;
        }

        uint64_t timeout_ticks =
            (uint64_t)timer_frequency() *
            AUTOTEST_SCHEDULER_TIMEOUT_SECONDS;

        if (
            now - state_started_at >=
            timeout_ticks
        )
        {
            report_result(
                "SCHEDULER_TIMEOUT",
                false
            );

            finish_tests();
        }
    }
}
