#ifndef ARP_H
#define ARP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void arp_init(uint32_t local_address);

void arp_receive(
    const uint8_t *packet,
    size_t length
);

bool arp_resolve(
    uint32_t address,
    uint8_t mac[6],
    uint32_t timeout_milliseconds
);

void arp_print_cache(void);

#endif
