#ifndef SECURITY_H
#define SECURITY_H

#include <stdbool.h>
#include <stdint.h>

#define SECURITY_USER_NAME_MAX 15
#define SECURITY_USER_COUNT    3

#define SECURITY_UID_ROOT  0U
#define SECURITY_GID_ROOT  0U
#define SECURITY_UID_USER  1000U
#define SECURITY_GID_USER  1000U
#define SECURITY_UID_GUEST 65534U
#define SECURITY_GID_GUEST 65534U

#define SECURITY_CAP_FILE_ADMIN     (1ULL << 0)
#define SECURITY_CAP_PROCESS_ADMIN  (1ULL << 1)
#define SECURITY_CAP_SYSTEM_CONTROL (1ULL << 2)
#define SECURITY_CAP_USER_ADMIN     (1ULL << 3)
#define SECURITY_CAP_PROCESS_SPAWN  (1ULL << 4)
#define SECURITY_CAP_NETWORK_USE    (1ULL << 5)
#define SECURITY_CAP_ALL            ((1ULL << 6) - 1ULL)

typedef struct
{
    uint32_t uid;
    uint32_t gid;
    const char *name;
    uint64_t capabilities;
} security_user_info_t;

void security_init(void);
bool security_is_ready(void);

bool security_login(
    const char *name,
    const char *password
);

void security_logout(void);

const char *security_current_username(void);
uint32_t security_session_uid(void);
uint32_t security_session_gid(void);
uint64_t security_session_capabilities(void);

uint32_t security_effective_uid(void);
uint32_t security_effective_gid(void);
uint64_t security_effective_capabilities(void);

bool security_has_capability(uint64_t capability);
bool security_effective_has_capability(
    uint64_t capability
);

uint32_t security_user_count(void);
bool security_user_get(
    uint32_t index,
    security_user_info_t *information
);

bool security_find_user(
    const char *name,
    security_user_info_t *information
);

bool security_find_user_by_uid(
    uint32_t uid,
    security_user_info_t *information
);

const char *security_user_name(uint32_t uid);

#endif
