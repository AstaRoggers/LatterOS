#ifndef INSTALLER_H
#define INSTALLER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define INSTALLER_MAX_TARGETS 8U
#define INSTALLER_NAME_CAPACITY 64U
#define INSTALLER_MESSAGE_CAPACITY 160U

typedef struct
{
    uint32_t target_index;
    uint32_t device_index;
    char name[INSTALLER_NAME_CAPACITY];
    uint32_t sector_size;
    uint64_t sector_count;
    uint64_t capacity_mib;
} installer_target_info_t;

typedef struct
{
    bool success;
    uint64_t efi_first_lba;
    uint64_t efi_last_lba;
    uint64_t system_first_lba;
    uint64_t system_last_lba;
    char message[INSTALLER_MESSAGE_CAPACITY];
} installer_report_t;

void installer_init(void);
void installer_refresh(void);
uint32_t installer_target_count(void);

bool installer_target_get(
    uint32_t target_index,
    installer_target_info_t *information
);

/* Synchronous path retained for tests and recovery tools. */
bool installer_prepare_target(
    uint32_t target_index,
    installer_report_t *report
);

/* Desktop path: runs destructive installation outside the GUI event loop. */
bool installer_start_target(uint32_t target_index);
bool installer_running(void);

bool installer_progress(
    uint32_t *percentage,
    char *message,
    size_t message_capacity
);

bool installer_take_completion(installer_report_t *report);

const char *installer_last_error(void);

#endif
