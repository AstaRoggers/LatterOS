#ifndef NVME_H
#define NVME_H

#include "block_device.h"

#include <stdbool.h>
#include <stdint.h>

bool nvme_init(void);
bool nvme_available(void);
uint32_t nvme_namespace_count(void);
const block_device_t *nvme_block_device(uint32_t index);
void nvme_print_status(void);
bool nvme_run_self_test(void);

#endif
