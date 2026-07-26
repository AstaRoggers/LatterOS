#ifndef PCI_H
#define PCI_H

#include <stdint.h>

#define PCI_MAX_DEVICES 1024

typedef struct
{
    uint8_t bus;
    uint8_t device;
    uint8_t function;

    uint16_t vendor_id;
    uint16_t device_id;

    uint8_t revision_id;
    uint8_t programming_interface;
    uint8_t subclass;
    uint8_t class_code;
    uint8_t header_type;

    uint32_t bars[6];
} pci_device_t;

void pci_init(void);

uint32_t pci_device_count(void);

const pci_device_t *pci_get_device(
    uint32_t index
);

const pci_device_t *pci_find_class(
    uint8_t class_code,
    uint8_t subclass,
    uint32_t occurrence
);

uint32_t pci_config_read32(
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint8_t offset
);

uint16_t pci_config_read16(
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint8_t offset
);

uint8_t pci_config_read8(
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint8_t offset
);

void pci_print_devices(void);

#endif
