#ifndef USB_H
#define USB_H

#include <stdbool.h>
#include <stdint.h>

#define USB_MAX_DEVICES 8
#define USB_MAX_CONFIGURATION_BYTES 256

typedef struct
{
    bool present;
    uint8_t port;
    uint8_t parent_address;
    uint8_t depth;
    uint8_t address;
    bool low_speed;
    bool is_hub;
    uint8_t hub_port_count;

    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t usb_version;
    uint16_t device_version;

    uint8_t device_class;
    uint8_t device_subclass;
    uint8_t device_protocol;
    uint8_t max_packet_size;
    uint8_t configuration_count;

    uint8_t configuration_value;
    uint8_t interface_count;
    uint8_t interface_number;
    uint8_t interface_class;
    uint8_t interface_subclass;
    uint8_t interface_protocol;

    uint8_t endpoint_count;
    uint8_t interrupt_endpoint;
    uint16_t interrupt_packet_size;
    uint8_t interrupt_interval;

    bool hid_boot_keyboard;
    bool hid_boot_mouse;

    bool mass_storage;
    uint8_t bulk_in_endpoint;
    uint8_t bulk_out_endpoint;
    uint16_t bulk_in_packet_size;
    uint16_t bulk_out_packet_size;

    uint64_t report_count;
} usb_device_t;

bool usb_init(void);
bool usb_is_ready(void);
void usb_poll(void);

uint8_t usb_connected_port_count(void);
uint8_t usb_device_count(void);
const usb_device_t *usb_device(uint8_t index);

bool usb_keyboard_ready(void);
bool usb_mouse_ready(void);

bool usb_control_request_device(
    const usb_device_t *device,
    uint8_t request_type,
    uint8_t request,
    uint16_t value,
    uint16_t index,
    void *data,
    uint16_t length
);

bool usb_bulk_transfer_device(
    const usb_device_t *device,
    uint8_t endpoint,
    bool direction_in,
    uint16_t packet_size,
    bool *data_toggle,
    void *data,
    uint16_t length
);

void usb_print_status(void);
void usb_print_devices(void);

#endif
