#ifndef STABILITY_MONITOR_H
#define STABILITY_MONITOR_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    STABILITY_STATE_IDLE,
    STABILITY_STATE_RUNNING,
    STABILITY_STATE_PASSED,
    STABILITY_STATE_WARNING,
    STABILITY_STATE_FAILED
} stability_state_t;

typedef struct
{
    stability_state_t state;
    uint32_t generation;
    uint32_t progress;
    uint64_t uptime_seconds;
    uint32_t timer_frequency;

    bool timer_tested;
    bool timer_passed;
    bool memory_tested;
    bool memory_passed;
    bool storage_tested;
    uint32_t online_block_devices;
    uint32_t invalid_block_devices;
    bool smp_tested;
    bool smp_submitted;
    bool smp_completed;
    uint32_t smp_cpu;
    uint64_t smp_job_id;
    bool display_tested;
    bool display_progressed;

    uint64_t display_presented_frames;
    uint64_t display_dropped_frames;
    uint64_t display_capture_failures;
    uint32_t warning_count;
    uint32_t failure_count;
} stability_snapshot_t;

void stability_monitor_init(void);

/* Returns true whenever visible state changed. */
bool stability_monitor_update(void);

bool stability_monitor_start_test(void);
void stability_monitor_reset_statistics(void);

const stability_snapshot_t *stability_monitor_snapshot(void);
stability_state_t stability_monitor_state(void);
const char *stability_monitor_state_name(void);
const char *stability_monitor_stage(void);
const char *stability_monitor_last_message(void);
bool stability_monitor_running(void);

bool stability_monitor_write_report(void);
const char *stability_monitor_report_path(void);

#endif
