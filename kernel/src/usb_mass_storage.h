#ifndef USB_MASS_STORAGE_H
#define USB_MASS_STORAGE_H

#include <stdbool.h>
#include <stdint.h>

#define USB_MASS_STORAGE_MAX_DEVICES 4

bool usb_mass_storage_init(void);
bool usb_mass_storage_ready(void);
uint8_t usb_mass_storage_count(void);
void usb_mass_storage_print(void);

#endif
