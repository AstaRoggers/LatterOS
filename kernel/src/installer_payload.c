#include "installer_payload.h"

#include <limine.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define INSTALLER_KERNEL_MODULE "latteros-installer-kernel"
#define INSTALLER_BOOTX64_MODULE "latteros-installer-bootx64"

__attribute__((used, section(".limine_requests")))
static volatile struct limine_module_request installer_module_request = {
    .id = LIMINE_MODULE_REQUEST_ID,
    .revision = 0
};

static bool strings_equal(
    const char *first,
    const char *second
)
{
    if (first == NULL || second == NULL)
    {
        return false;
    }

    size_t index = 0;

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

static const char *module_name(installer_payload_kind_t kind)
{
    switch (kind)
    {
        case INSTALLER_PAYLOAD_KERNEL:
            return INSTALLER_KERNEL_MODULE;

        case INSTALLER_PAYLOAD_BOOTX64:
            return INSTALLER_BOOTX64_MODULE;

        default:
            return NULL;
    }
}

bool installer_payload_get(
    installer_payload_kind_t kind,
    const uint8_t **data,
    size_t *size
)
{
    if (data == NULL || size == NULL)
    {
        return false;
    }

    *data = NULL;
    *size = 0;

    const char *expected = module_name(kind);
    struct limine_module_response *response =
        installer_module_request.response;

    if (
        expected == NULL ||
        response == NULL ||
        response->modules == NULL
    )
    {
        return false;
    }

    for (
        uint64_t index = 0;
        index < response->module_count;
        index++
    )
    {
        struct limine_file *module = response->modules[index];

        if (
            module == NULL ||
            module->address == NULL ||
            module->size == 0 ||
            !strings_equal(module->string, expected) ||
            module->size > SIZE_MAX
        )
        {
            continue;
        }

        *data = module->address;
        *size = (size_t)module->size;
        return true;
    }

    return false;
}

bool installer_payload_available(void)
{
    const uint8_t *kernel;
    const uint8_t *bootx64;
    size_t kernel_size;
    size_t bootx64_size;

    return
        installer_payload_get(
            INSTALLER_PAYLOAD_KERNEL,
            &kernel,
            &kernel_size
        ) &&
        installer_payload_get(
            INSTALLER_PAYLOAD_BOOTX64,
            &bootx64,
            &bootx64_size
        ) &&
        kernel != NULL &&
        bootx64 != NULL &&
        kernel_size != 0 &&
        bootx64_size != 0;
}
