#ifndef INSTALLER_PAYLOAD_H
#define INSTALLER_PAYLOAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum
{
    INSTALLER_PAYLOAD_KERNEL,
    INSTALLER_PAYLOAD_BOOTX64
} installer_payload_kind_t;

bool installer_payload_get(
    installer_payload_kind_t kind,
    const uint8_t **data,
    size_t *size
);

bool installer_payload_available(void);

#endif
