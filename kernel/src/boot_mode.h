#ifndef BOOT_MODE_H
#define BOOT_MODE_H

#include <stdbool.h>

typedef enum
{
    BOOT_MODE_NORMAL,
    BOOT_MODE_SAFE,
    BOOT_MODE_RECOVERY,
    BOOT_MODE_HARDWARE_TEST,
    BOOT_MODE_COMPATIBILITY,
    BOOT_MODE_VIRTUALBOX
} boot_mode_kind_t;

typedef enum
{
    BOOT_SOURCE_UNKNOWN,
    BOOT_SOURCE_INSTALLER,
    BOOT_SOURCE_INSTALLED,
    BOOT_SOURCE_RECOVERY_MEDIA
} boot_source_kind_t;

void boot_mode_init(void);
boot_mode_kind_t boot_mode_kind(void);
boot_source_kind_t boot_mode_source(void);

bool boot_mode_is_safe(void);
bool boot_mode_is_recovery(void);
bool boot_mode_is_hardware_test(void);
bool boot_mode_is_compatibility(void);
bool boot_mode_is_virtualbox(void);
bool boot_mode_is_installed(void);
bool boot_mode_conservative_graphics(void);
const char *boot_mode_name(void);
const char *boot_source_name(void);

/* Call after /home/user exists. Returns true only on the first installed boot. */
bool boot_mode_prepare_first_boot(void);
bool boot_mode_first_boot(void);

#endif
