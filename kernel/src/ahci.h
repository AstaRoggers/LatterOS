#ifndef AHCI_H
#define AHCI_H

#include "block_device.h"

#include <stdbool.h>
#include <stdint.h>

bool ahci_init(void);
bool ahci_available(void);
uint32_t ahci_device_count(void);
const block_device_t *ahci_block_device(uint32_t index);
void ahci_print_status(void);
bool ahci_run_self_test(void);

#endif
