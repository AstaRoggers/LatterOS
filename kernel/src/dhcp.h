#ifndef DHCP_H
#define DHCP_H

#include <stdbool.h>
#include <stdint.h>

void dhcp_init(void);

bool dhcp_configure(
    uint32_t timeout_iterations
);

bool dhcp_is_configured(void);
uint32_t dhcp_dns_server(void);
uint32_t dhcp_lease_seconds(void);

void dhcp_print_status(void);

#endif
