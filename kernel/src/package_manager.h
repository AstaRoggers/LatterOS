#ifndef PACKAGE_MANAGER_H
#define PACKAGE_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#define PACKAGE_NAME_MAX 31U
#define PACKAGE_VERSION_MAX 31U
#define PACKAGE_DESCRIPTION_MAX 127U
#define PACKAGE_DEPENDS_MAX 127U
#define PACKAGE_ENTRY_MAX 127U

typedef struct
{
    char name[PACKAGE_NAME_MAX + 1U];
    char version[PACKAGE_VERSION_MAX + 1U];
    char description[PACKAGE_DESCRIPTION_MAX + 1U];
    char depends[PACKAGE_DEPENDS_MAX + 1U];
    char entry[PACKAGE_ENTRY_MAX + 1U];
    uint32_t file_count;
    uint32_t payload_size;
    bool installed;
} package_info_t;

typedef enum
{
    PACKAGE_RESULT_OK,
    PACKAGE_RESULT_INVALID_ARGUMENT,
    PACKAGE_RESULT_NOT_FOUND,
    PACKAGE_RESULT_BAD_ARCHIVE,
    PACKAGE_RESULT_UNSUPPORTED_VERSION,
    PACKAGE_RESULT_CHECKSUM_FAILED,
    PACKAGE_RESULT_INVALID_MANIFEST,
    PACKAGE_RESULT_INVALID_NAME,
    PACKAGE_RESULT_INVALID_PATH,
    PACKAGE_RESULT_DEPENDENCY_MISSING,
    PACKAGE_RESULT_DEPENDENCY_IN_USE,
    PACKAGE_RESULT_DOWNGRADE_BLOCKED,
    PACKAGE_RESULT_FILESYSTEM_ERROR,
    PACKAGE_RESULT_NOT_INSTALLED
} package_result_t;

void package_manager_init(void);

package_result_t package_manager_probe(
    const char *archive_path,
    package_info_t *information
);

package_result_t package_manager_install(
    const char *archive_path,
    package_info_t *installed_information
);

package_result_t package_manager_remove(const char *name);

bool package_manager_is_installed(const char *name);
uint32_t package_manager_installed_count(void);

bool package_manager_installed_get(
    uint32_t index,
    package_info_t *information
);

const char *package_result_message(package_result_t result);

#endif
