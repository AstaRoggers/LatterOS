#include "security.h"

#include "process.h"
#include "vfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct
{
    uint32_t uid;
    uint32_t gid;
    const char *name;
    const char *password;
    uint64_t capabilities;
} security_account_t;

static const security_account_t accounts[
    SECURITY_USER_COUNT
] = {
    {
        .uid = SECURITY_UID_ROOT,
        .gid = SECURITY_GID_ROOT,
        .name = "root",
        .password = "root",
        .capabilities = SECURITY_CAP_ALL
    },
    {
        .uid = SECURITY_UID_USER,
        .gid = SECURITY_GID_USER,
        .name = "user",
        .password = "latteros",
        .capabilities =
            SECURITY_CAP_SYSTEM_CONTROL |
            SECURITY_CAP_PROCESS_SPAWN |
            SECURITY_CAP_NETWORK_USE
    },
    {
        .uid = SECURITY_UID_GUEST,
        .gid = SECURITY_GID_GUEST,
        .name = "guest",
        .password = "guest",
        .capabilities = SECURITY_CAP_NETWORK_USE
    }
};

static bool ready;
static uint32_t current_account_index;

static bool strings_equal(
    const char *first,
    const char *second
)
{
    if (
        first == NULL ||
        second == NULL
    )
    {
        return false;
    }

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

static const security_account_t *account_by_uid(
    uint32_t uid
)
{
    for (
        uint32_t index = 0;
        index < SECURITY_USER_COUNT;
        index++
    )
    {
        if (accounts[index].uid == uid)
        {
            return &accounts[index];
        }
    }

    return NULL;
}

void security_init(void)
{
    current_account_index = 0;
    ready = true;

    if (vfs_open("/home/guest") == NULL)
    {
        (void)vfs_make_directory(
            "/home/guest"
        );
    }

    (void)vfs_chown(
        "/home/guest",
        SECURITY_UID_GUEST,
        SECURITY_GID_GUEST
    );

    (void)vfs_chmod(
        "/home/guest",
        0700U
    );

    current_account_index = 1;
}

bool security_is_ready(void)
{
    return ready;
}

bool security_login(
    const char *name,
    const char *password
)
{
    if (
        !ready ||
        name == NULL ||
        password == NULL
    )
    {
        return false;
    }

    for (
        uint32_t index = 0;
        index < SECURITY_USER_COUNT;
        index++
    )
    {
        if (
            strings_equal(
                accounts[index].name,
                name
            ) &&
            strings_equal(
                accounts[index].password,
                password
            )
        )
        {
            current_account_index = index;
            return true;
        }
    }

    return false;
}

void security_logout(void)
{
    if (!ready)
    {
        return;
    }

    current_account_index = 2;
}

const char *security_current_username(void)
{
    if (!ready)
    {
        return "kernel";
    }

    return accounts[current_account_index].name;
}

uint32_t security_session_uid(void)
{
    if (!ready)
    {
        return SECURITY_UID_ROOT;
    }

    return accounts[current_account_index].uid;
}

uint32_t security_session_gid(void)
{
    if (!ready)
    {
        return SECURITY_GID_ROOT;
    }

    return accounts[current_account_index].gid;
}

uint64_t security_session_capabilities(void)
{
    if (!ready)
    {
        return SECURITY_CAP_ALL;
    }

    return accounts[current_account_index].capabilities;
}

uint32_t security_effective_uid(void)
{
    const process_t *process = process_current();

    if (
        process != NULL &&
        process->mode == PROCESS_USER
    )
    {
        return process->uid;
    }

    return security_session_uid();
}

uint32_t security_effective_gid(void)
{
    const process_t *process = process_current();

    if (
        process != NULL &&
        process->mode == PROCESS_USER
    )
    {
        return process->gid;
    }

    return security_session_gid();
}

uint64_t security_effective_capabilities(void)
{
    const process_t *process = process_current();

    if (
        process != NULL &&
        process->mode == PROCESS_USER
    )
    {
        return process->capabilities;
    }

    return security_session_capabilities();
}

bool security_has_capability(uint64_t capability)
{
    return (
        security_session_capabilities() &
        capability
    ) == capability;
}

bool security_effective_has_capability(
    uint64_t capability
)
{
    return (
        security_effective_capabilities() &
        capability
    ) == capability;
}

uint32_t security_user_count(void)
{
    return SECURITY_USER_COUNT;
}

bool security_user_get(
    uint32_t index,
    security_user_info_t *information
)
{
    if (
        information == NULL ||
        index >= SECURITY_USER_COUNT
    )
    {
        return false;
    }

    information->uid = accounts[index].uid;
    information->gid = accounts[index].gid;
    information->name = accounts[index].name;
    information->capabilities =
        accounts[index].capabilities;

    return true;
}

bool security_find_user(
    const char *name,
    security_user_info_t *information
)
{
    if (
        name == NULL ||
        information == NULL
    )
    {
        return false;
    }

    for (
        uint32_t index = 0;
        index < SECURITY_USER_COUNT;
        index++
    )
    {
        if (strings_equal(accounts[index].name, name))
        {
            return security_user_get(
                index,
                information
            );
        }
    }

    return false;
}

bool security_find_user_by_uid(
    uint32_t uid,
    security_user_info_t *information
)
{
    if (information == NULL)
    {
        return false;
    }

    const security_account_t *account =
        account_by_uid(uid);

    if (account == NULL)
    {
        return false;
    }

    information->uid = account->uid;
    information->gid = account->gid;
    information->name = account->name;
    information->capabilities =
        account->capabilities;

    return true;
}

const char *security_user_name(uint32_t uid)
{
    const security_account_t *account =
        account_by_uid(uid);

    return account != NULL ?
        account->name :
        "unknown";
}
