#include "stability_monitor.h"

#include "block_device.h"
#include "boot_mode.h"
#include "display.h"
#include "hhdm.h"
#include "page_allocator.h"
#include "physical_memory.h"
#include "smp.h"
#include "smp_scheduler.h"
#include "timer.h"
#include "vfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STABILITY_REPORT_PATH \
    "/home/user/Documents/System Stability Report.txt"
#define STABILITY_REPORT_CAPACITY 16384U
#define STABILITY_SMP_MAGIC 0x4C4154544552534FULL
#define STABILITY_PAGE_SIZE 4096U

typedef enum
{
    TEST_STEP_IDLE,
    TEST_STEP_TIMER_WAIT,
    TEST_STEP_MEMORY,
    TEST_STEP_STORAGE,
    TEST_STEP_SMP_SUBMIT,
    TEST_STEP_SMP_WAIT,
    TEST_STEP_DISPLAY_WAIT,
    TEST_STEP_FINISH
} test_step_t;

static stability_snapshot_t snapshot;
static bool initialized;
static test_step_t test_step;
static uint64_t initialized_tick;
static uint64_t step_started_tick;
static uint64_t timer_baseline;
static uint64_t display_baseline;
static volatile uint64_t smp_completion_value;
static char stage_text[96];
static char last_message[128];
static char report_buffer[STABILITY_REPORT_CAPACITY];

static void clear_bytes(void *pointer, size_t count)
{
    uint8_t *bytes = pointer;

    for (size_t index = 0; index < count; index++)
    {
        bytes[index] = 0;
    }
}

static void copy_text(
    char *destination,
    size_t capacity,
    const char *source
)
{
    if (destination == NULL || capacity == 0U)
    {
        return;
    }

    size_t index = 0U;

    if (source != NULL)
    {
        while (source[index] != '\0' && index + 1U < capacity)
        {
            destination[index] = source[index];
            index++;
        }
    }

    destination[index] = '\0';
}

static void append_character(
    char *buffer,
    size_t capacity,
    size_t *position,
    char character
)
{
    if (
        buffer == NULL ||
        position == NULL ||
        *position + 1U >= capacity
    )
    {
        return;
    }

    buffer[*position] = character;
    (*position)++;
    buffer[*position] = '\0';
}

static void append_text(
    char *buffer,
    size_t capacity,
    size_t *position,
    const char *text
)
{
    if (text == NULL)
    {
        return;
    }

    for (size_t index = 0U; text[index] != '\0'; index++)
    {
        append_character(buffer, capacity, position, text[index]);
    }
}

static void append_unsigned(
    char *buffer,
    size_t capacity,
    size_t *position,
    uint64_t value
)
{
    char reverse[24];
    uint32_t count = 0U;

    do
    {
        reverse[count++] = (char)('0' + value % 10ULL);
        value /= 10ULL;
    }
    while (value != 0ULL && count < sizeof(reverse));

    while (count > 0U)
    {
        append_character(
            buffer,
            capacity,
            position,
            reverse[--count]
        );
    }
}

static void append_boolean(
    char *buffer,
    size_t capacity,
    size_t *position,
    bool value
)
{
    append_text(
        buffer,
        capacity,
        position,
        value ? "yes" : "no"
    );
}

static uint64_t ticks_for_milliseconds(uint32_t milliseconds)
{
    uint64_t frequency = timer_frequency();

    if (frequency == 0ULL)
    {
        return 1ULL;
    }

    uint64_t ticks = (frequency * milliseconds) / 1000ULL;
    return ticks == 0ULL ? 1ULL : ticks;
}

static void set_stage(uint32_t progress, const char *text)
{
    snapshot.progress = progress;
    copy_text(stage_text, sizeof(stage_text), text);
    snapshot.generation++;
}

static void add_warning(void)
{
    snapshot.warning_count++;
}

static void add_failure(void)
{
    snapshot.failure_count++;
}

static uint64_t stability_smp_job(void *argument)
{
    volatile uint64_t *completion = argument;

    if (completion != NULL)
    {
        __atomic_store_n(
            completion,
            STABILITY_SMP_MAGIC,
            __ATOMIC_RELEASE
        );
    }

    return STABILITY_SMP_MAGIC;
}

static bool run_memory_test(void)
{
    /*
     * alloc_page() returns a physical address. The kernel does not identity-map
     * all usable RAM, so dereferencing that address directly can page-fault.
     * Access the allocated page through Limine's higher-half direct map and
     * return the original physical address to the allocator.
     */
    void *physical_page = alloc_page();

    if (physical_page == NULL)
    {
        return false;
    }

    uint8_t *page = physical_to_virtual(
        (uint64_t)(uintptr_t)physical_page
    );

    if (page == NULL)
    {
        free_page(physical_page);
        return false;
    }

    for (uint32_t index = 0U; index < STABILITY_PAGE_SIZE; index++)
    {
        page[index] = (uint8_t)(
            (index * 37U + 0x5AU) & 0xFFU
        );
    }

    bool passed = true;

    for (uint32_t index = 0U; index < STABILITY_PAGE_SIZE; index++)
    {
        uint8_t expected = (uint8_t)(
            (index * 37U + 0x5AU) & 0xFFU
        );

        if (page[index] != expected)
        {
            passed = false;
            break;
        }
    }

    free_page(physical_page);
    return passed;
}

static void inspect_block_devices(void)
{
    snapshot.online_block_devices = 0U;
    snapshot.invalid_block_devices = 0U;

    uint32_t count = block_device_count();

    for (uint32_t index = 0U; index < count; index++)
    {
        const block_device_t *device = block_device_get(index);

        if (device == NULL || !device->online)
        {
            continue;
        }

        snapshot.online_block_devices++;

        if (
            device->name == NULL ||
            device->sector_size == 0U ||
            device->sector_count == 0ULL ||
            device->read == NULL
        )
        {
            snapshot.invalid_block_devices++;
        }
    }
}

static void finish_test(void)
{
    snapshot.progress = 100U;

    if (snapshot.failure_count != 0U)
    {
        snapshot.state = STABILITY_STATE_FAILED;
        copy_text(stage_text, sizeof(stage_text), "Stability test failed");
        copy_text(
            last_message,
            sizeof(last_message),
            "Stability test found a critical failure"
        );
    }
    else if (snapshot.warning_count != 0U)
    {
        snapshot.state = STABILITY_STATE_WARNING;
        copy_text(stage_text, sizeof(stage_text), "Completed with warnings");
        copy_text(
            last_message,
            sizeof(last_message),
            "Stability test completed with warnings"
        );
    }
    else
    {
        snapshot.state = STABILITY_STATE_PASSED;
        copy_text(stage_text, sizeof(stage_text), "All stability checks passed");
        copy_text(
            last_message,
            sizeof(last_message),
            "Stability test passed"
        );
    }

    test_step = TEST_STEP_IDLE;
    snapshot.generation++;
}

void stability_monitor_init(void)
{
    if (initialized)
    {
        return;
    }

    clear_bytes(&snapshot, sizeof(snapshot));
    initialized_tick = timer_ticks();
    snapshot.state = STABILITY_STATE_IDLE;
    snapshot.timer_frequency = timer_frequency();
    copy_text(stage_text, sizeof(stage_text), "Ready for non-destructive testing");
    copy_text(last_message, sizeof(last_message), "Stability monitor ready");
    test_step = TEST_STEP_IDLE;
    initialized = true;
}

bool stability_monitor_start_test(void)
{
    stability_monitor_init();

    if (snapshot.state == STABILITY_STATE_RUNNING)
    {
        copy_text(last_message, sizeof(last_message), "Stability test already running");
        return false;
    }

    uint32_t next_generation = snapshot.generation + 1U;
    clear_bytes(&snapshot, sizeof(snapshot));
    snapshot.generation = next_generation;
    snapshot.state = STABILITY_STATE_RUNNING;
    snapshot.timer_frequency = timer_frequency();
    timer_baseline = timer_ticks();
    step_started_tick = timer_baseline;
    display_stats_t statistics;
    clear_bytes(&statistics, sizeof(statistics));
    display_get_stats(&statistics);
    display_baseline = statistics.presented_frames;
    smp_completion_value = 0ULL;
    test_step = TEST_STEP_TIMER_WAIT;
    set_stage(8U, "Checking timer progression");
    copy_text(last_message, sizeof(last_message), "Stability test started");
    return true;
}

bool stability_monitor_update(void)
{
    stability_monitor_init();

    uint64_t now = timer_ticks();
    uint32_t frequency = timer_frequency();
    uint64_t uptime = 0ULL;

    if (frequency != 0U && now >= initialized_tick)
    {
        uptime = (now - initialized_tick) / frequency;
    }

    bool visible_change = uptime != snapshot.uptime_seconds;
    snapshot.uptime_seconds = uptime;
    snapshot.timer_frequency = frequency;

    display_stats_t statistics;
    clear_bytes(&statistics, sizeof(statistics));
    display_get_stats(&statistics);

    if (
        statistics.presented_frames != snapshot.display_presented_frames ||
        statistics.dropped_frames != snapshot.display_dropped_frames ||
        statistics.capture_failures != snapshot.display_capture_failures
    )
    {
        snapshot.display_presented_frames = statistics.presented_frames;
        snapshot.display_dropped_frames = statistics.dropped_frames;
        snapshot.display_capture_failures = statistics.capture_failures;
        visible_change = true;
    }

    if (snapshot.state != STABILITY_STATE_RUNNING)
    {
        return visible_change;
    }

    switch (test_step)
    {
        case TEST_STEP_TIMER_WAIT:
            if (
                now - step_started_tick >=
                    ticks_for_milliseconds(150U)
            )
            {
                snapshot.timer_tested = true;
                snapshot.timer_passed =
                    frequency != 0U && now > timer_baseline;

                if (!snapshot.timer_passed)
                {
                    add_failure();
                }

                test_step = TEST_STEP_MEMORY;
                set_stage(25U, "Testing one page of physical memory");
                visible_change = true;
            }
            break;

        case TEST_STEP_MEMORY:
            snapshot.memory_tested = true;
            snapshot.memory_passed = run_memory_test();

            if (!snapshot.memory_passed)
            {
                add_failure();
            }

            test_step = TEST_STEP_STORAGE;
            set_stage(42U, "Validating block-device registrations");
            visible_change = true;
            break;

        case TEST_STEP_STORAGE:
            inspect_block_devices();
            snapshot.storage_tested = true;

            if (snapshot.invalid_block_devices != 0U)
            {
                add_failure();
            }
            else if (snapshot.online_block_devices == 0U)
            {
                add_warning();
            }

            test_step = TEST_STEP_SMP_SUBMIT;
            set_stage(58U, "Checking application-processor scheduling");
            visible_change = true;
            break;

        case TEST_STEP_SMP_SUBMIT:
            snapshot.smp_tested = true;
            snapshot.smp_cpu = UINT32_MAX;
            snapshot.smp_submitted = smp_scheduler_submit_any(
                stability_smp_job,
                (void *)&smp_completion_value,
                &snapshot.smp_cpu,
                &snapshot.smp_job_id
            );

            step_started_tick = now;

            if (!snapshot.smp_submitted)
            {
                add_warning();
                test_step = TEST_STEP_DISPLAY_WAIT;
                display_baseline = statistics.presented_frames;
                step_started_tick = now;
                set_stage(76U, "Checking display presentation");
            }
            else
            {
                test_step = TEST_STEP_SMP_WAIT;
                set_stage(68U, "Waiting for application-processor result");
            }

            visible_change = true;
            break;

        case TEST_STEP_SMP_WAIT:
            if (
                __atomic_load_n(
                    &smp_completion_value,
                    __ATOMIC_ACQUIRE
                ) == STABILITY_SMP_MAGIC
            )
            {
                snapshot.smp_completed = true;
                test_step = TEST_STEP_DISPLAY_WAIT;
                display_baseline = statistics.presented_frames;
                step_started_tick = now;
                set_stage(76U, "Checking display presentation");
                visible_change = true;
            }
            else if (
                now - step_started_tick >=
                    ticks_for_milliseconds(2000U)
            )
            {
                add_warning();
                test_step = TEST_STEP_DISPLAY_WAIT;
                display_baseline = statistics.presented_frames;
                step_started_tick = now;
                set_stage(76U, "Checking display presentation");
                visible_change = true;
            }
            break;

        case TEST_STEP_DISPLAY_WAIT:
            if (
                now - step_started_tick >=
                    ticks_for_milliseconds(650U)
            )
            {
                snapshot.display_tested = true;
                snapshot.display_progressed =
                    statistics.presented_frames > display_baseline;

                if (!snapshot.display_progressed)
                {
                    add_warning();
                }

                if (statistics.capture_failures != 0ULL)
                {
                    add_warning();
                }

                test_step = TEST_STEP_FINISH;
                set_stage(94U, "Finalizing stability result");
                visible_change = true;
            }
            break;

        case TEST_STEP_FINISH:
            finish_test();
            visible_change = true;
            break;

        case TEST_STEP_IDLE:
        default:
            break;
    }

    return visible_change;
}

void stability_monitor_reset_statistics(void)
{
    stability_monitor_init();

    if (snapshot.state == STABILITY_STATE_RUNNING)
    {
        copy_text(last_message, sizeof(last_message), "Cannot reset while a test is running");
        return;
    }

    display_reset_statistics();
    uint32_t next_generation = snapshot.generation + 1U;
    clear_bytes(&snapshot, sizeof(snapshot));
    snapshot.generation = next_generation;
    snapshot.state = STABILITY_STATE_IDLE;
    snapshot.timer_frequency = timer_frequency();
    initialized_tick = timer_ticks();
    copy_text(stage_text, sizeof(stage_text), "Ready for non-destructive testing");
    copy_text(last_message, sizeof(last_message), "Stability statistics reset");
}

const stability_snapshot_t *stability_monitor_snapshot(void)
{
    stability_monitor_init();
    return &snapshot;
}

stability_state_t stability_monitor_state(void)
{
    return stability_monitor_snapshot()->state;
}

const char *stability_monitor_state_name(void)
{
    switch (stability_monitor_state())
    {
        case STABILITY_STATE_RUNNING:
            return "RUNNING";

        case STABILITY_STATE_PASSED:
            return "STABLE";

        case STABILITY_STATE_WARNING:
            return "WARNING";

        case STABILITY_STATE_FAILED:
            return "FAILED";

        case STABILITY_STATE_IDLE:
        default:
            return "READY";
    }
}

const char *stability_monitor_stage(void)
{
    stability_monitor_init();
    return stage_text;
}

const char *stability_monitor_last_message(void)
{
    stability_monitor_init();
    return last_message;
}

bool stability_monitor_running(void)
{
    return stability_monitor_state() == STABILITY_STATE_RUNNING;
}

bool stability_monitor_write_report(void)
{
    stability_monitor_init();

    size_t position = 0U;
    report_buffer[0] = '\0';

    append_text(report_buffer, sizeof(report_buffer), &position,
        "LatterOS System Stability Report\n"
        "Milestone 20B\n\n"
        "Boot profile: ");
    append_text(report_buffer, sizeof(report_buffer), &position, boot_mode_name());
    append_text(report_buffer, sizeof(report_buffer), &position, "\nBoot source: ");
    append_text(report_buffer, sizeof(report_buffer), &position, boot_source_name());
    append_text(report_buffer, sizeof(report_buffer), &position, "\nState: ");
    append_text(report_buffer, sizeof(report_buffer), &position, stability_monitor_state_name());
    append_text(report_buffer, sizeof(report_buffer), &position, "\nStage: ");
    append_text(report_buffer, sizeof(report_buffer), &position, stability_monitor_stage());
    append_text(report_buffer, sizeof(report_buffer), &position, "\nProgress: ");
    append_unsigned(report_buffer, sizeof(report_buffer), &position, snapshot.progress);
    append_text(report_buffer, sizeof(report_buffer), &position, "%\nUptime seconds: ");
    append_unsigned(report_buffer, sizeof(report_buffer), &position, snapshot.uptime_seconds);
    append_text(report_buffer, sizeof(report_buffer), &position, "\nTimer frequency: ");
    append_unsigned(report_buffer, sizeof(report_buffer), &position, snapshot.timer_frequency);
    append_text(report_buffer, sizeof(report_buffer), &position, " Hz\n\nChecks\n------\nTimer tested/passed: ");
    append_boolean(report_buffer, sizeof(report_buffer), &position, snapshot.timer_tested);
    append_character(report_buffer, sizeof(report_buffer), &position, '/');
    append_boolean(report_buffer, sizeof(report_buffer), &position, snapshot.timer_passed);
    append_text(report_buffer, sizeof(report_buffer), &position, "\nMemory tested/passed: ");
    append_boolean(report_buffer, sizeof(report_buffer), &position, snapshot.memory_tested);
    append_character(report_buffer, sizeof(report_buffer), &position, '/');
    append_boolean(report_buffer, sizeof(report_buffer), &position, snapshot.memory_passed);
    append_text(report_buffer, sizeof(report_buffer), &position, "\nStorage metadata tested: ");
    append_boolean(report_buffer, sizeof(report_buffer), &position, snapshot.storage_tested);
    append_text(report_buffer, sizeof(report_buffer), &position, "\nOnline/invalid block devices: ");
    append_unsigned(report_buffer, sizeof(report_buffer), &position, snapshot.online_block_devices);
    append_character(report_buffer, sizeof(report_buffer), &position, '/');
    append_unsigned(report_buffer, sizeof(report_buffer), &position, snapshot.invalid_block_devices);
    append_text(report_buffer, sizeof(report_buffer), &position, "\nSMP tested/submitted/completed: ");
    append_boolean(report_buffer, sizeof(report_buffer), &position, snapshot.smp_tested);
    append_character(report_buffer, sizeof(report_buffer), &position, '/');
    append_boolean(report_buffer, sizeof(report_buffer), &position, snapshot.smp_submitted);
    append_character(report_buffer, sizeof(report_buffer), &position, '/');
    append_boolean(report_buffer, sizeof(report_buffer), &position, snapshot.smp_completed);
    append_text(report_buffer, sizeof(report_buffer), &position, "\nSMP CPU/job: ");
    append_unsigned(report_buffer, sizeof(report_buffer), &position, snapshot.smp_cpu);
    append_character(report_buffer, sizeof(report_buffer), &position, '/');
    append_unsigned(report_buffer, sizeof(report_buffer), &position, snapshot.smp_job_id);
    append_text(report_buffer, sizeof(report_buffer), &position, "\nDisplay tested/progressed: ");
    append_boolean(report_buffer, sizeof(report_buffer), &position, snapshot.display_tested);
    append_character(report_buffer, sizeof(report_buffer), &position, '/');
    append_boolean(report_buffer, sizeof(report_buffer), &position, snapshot.display_progressed);
    append_text(report_buffer, sizeof(report_buffer), &position, "\nPresented/dropped/capture failures: ");
    append_unsigned(report_buffer, sizeof(report_buffer), &position, snapshot.display_presented_frames);
    append_character(report_buffer, sizeof(report_buffer), &position, '/');
    append_unsigned(report_buffer, sizeof(report_buffer), &position, snapshot.display_dropped_frames);
    append_character(report_buffer, sizeof(report_buffer), &position, '/');
    append_unsigned(report_buffer, sizeof(report_buffer), &position, snapshot.display_capture_failures);
    append_text(report_buffer, sizeof(report_buffer), &position, "\nWarnings/failures: ");
    append_unsigned(report_buffer, sizeof(report_buffer), &position, snapshot.warning_count);
    append_character(report_buffer, sizeof(report_buffer), &position, '/');
    append_unsigned(report_buffer, sizeof(report_buffer), &position, snapshot.failure_count);
    append_text(report_buffer, sizeof(report_buffer), &position,
        "\n\nNotes\n-----\n"
        "The stability test is non-destructive. It validates timer progression,\n"
        "one allocated memory page, registered block-device metadata, an AP\n"
        "scheduler job when available, and active display presentation.\n"
        "Compatibility Mode uses conservative software graphics.\n");

    bool written = vfs_write_text(STABILITY_REPORT_PATH, report_buffer);
    copy_text(
        last_message,
        sizeof(last_message),
        written ?
            "System stability report saved in Documents" :
            "Unable to save system stability report"
    );
    snapshot.generation++;
    return written;
}

const char *stability_monitor_report_path(void)
{
    return STABILITY_REPORT_PATH;
}
