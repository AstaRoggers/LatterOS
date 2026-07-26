#ifndef ATA_H
#define ATA_H

#include "block_device.h"

#include <stdbool.h>

bool ata_init(void);
bool ata_available(void);

const block_device_t *ata_block_device(void);

#endif
