#ifndef USB_H
#define USB_H

#include <stdbool.h>
#include <stdint.h>

bool usb_init(void);
bool usb_is_ready(void);
uint8_t usb_connected_port_count(void);
void usb_print_status(void);

#endif
