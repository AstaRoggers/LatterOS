#ifndef USB_HOTPLUG_H
#define USB_HOTPLUG_H

#include <stdbool.h>
#include <stdint.h>

bool usb_hotplug_init(void);

uint64_t usb_hotplug_event_count(void);
uint64_t usb_hotplug_removal_count(void);
uint64_t usb_hotplug_insertion_count(void);
uint64_t usb_hotplug_hub_event_count(void);

#endif
