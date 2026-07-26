#ifndef DNS_H
#define DNS_H

#include <stdbool.h>
#include <stdint.h>

void dns_init(uint32_t server_address);
void dns_set_server(uint32_t server_address);
uint32_t dns_server(void);

bool dns_resolve_a(
    const char *name,
    uint32_t timeout_iterations,
    uint32_t *address
);

void dns_print_server(void);

#endif
