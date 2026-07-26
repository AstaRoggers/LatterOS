#ifndef STORAGE_H
#define STORAGE_H

#include "block_device.h"
#include "pci.h"

#include <stdbool.h>

typedef enum
{
    STORAGE_CONTROLLER_NONE,
    STORAGE_CONTROLLER_IDE,
    STORAGE_CONTROLLER_AHCI,
    STORAGE_CONTROLLER_NVME,
    STORAGE_CONTROLLER_OTHER
} storage_controller_type_t;

void storage_init(void);

bool storage_controller_found(void);

storage_controller_type_t storage_controller_type(void);

const pci_device_t *storage_controller_device(void);

const block_device_t *storage_primary_device(void);

void storage_print_controller(void);
void storage_print_devices(void);

#endif
